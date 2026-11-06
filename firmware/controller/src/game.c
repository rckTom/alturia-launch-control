/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * Stacker, see game.h. Runs in the LVGL thread, one step per controller
 * tick (50 ms), so every time below is a multiple of that in practice.
 *
 * Button presses are taken from the press counters of inputs_diag()
 * rather than from the controller's request queue: a counter does not
 * miss a tap shorter than a tick, and it has no auto-repeat, so holding
 * up turns a shape once. Held buttons (left, right, down) are read as
 * levels.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "game.h"
#include "inputs.h"

/* Held left or right: first repeat after this, then one column per tick */
#define REPEAT_DELAY_MS 170
#define REPEAT_MS       50
/* Held down: one row per tick */
#define SOFT_DROP_MS    50
/* A shape that touches down can still be moved for this long... */
#define LOCK_MS         500
/* ...but moving it restarts that only so often */
#define LOCK_RESETS     15
/* After the game ends, OK starts a new one only after this */
#define OVER_GUARD_MS   800
/* A longer gap between ticks: the screen was left, the game paused */
#define PAUSE_GAP_MS    300

/* Rows per level, and the time a shape takes to fall one row */
#define LINES_PER_LEVEL 10
static const uint16_t fall_ms[] = {
	800, 680, 580, 490, 420, 355, 300, 255, 220, 185, 160, 135, 115, 100, 85, 70, 60, 50,
};

/* Points for 1, 2, 3 and 4 rows at once, times (level + 1) */
static const uint16_t row_points[] = {0, 100, 300, 500, 800};

/* The seven shapes: four blocks each in an n x n box, x right, y down */
struct shape {
	uint8_t n;
	int8_t spawn_y;   /* so that the top block starts in row 0 */
	uint8_t cell[4][2];
};

static const struct shape shapes[] = {
	{4, -1, {{0, 1}, {1, 1}, {2, 1}, {3, 1}}},   /* I */
	{2, 0, {{0, 0}, {1, 0}, {0, 1}, {1, 1}}},    /* O */
	{3, 0, {{1, 0}, {0, 1}, {1, 1}, {2, 1}}},    /* T */
	{3, 0, {{1, 0}, {2, 0}, {0, 1}, {1, 1}}},    /* S */
	{3, 0, {{0, 0}, {1, 0}, {1, 1}, {2, 1}}},    /* Z */
	{3, 0, {{0, 0}, {0, 1}, {1, 1}, {2, 1}}},    /* J */
	{3, 0, {{2, 0}, {0, 1}, {1, 1}, {2, 1}}},    /* L */
};

#define NUM_SHAPES ARRAY_SIZE(shapes)

/* Turning against a wall or the floor: the offsets tried, in order */
static const int8_t kicks[][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, -1}, {2, 0}, {-2, 0}};

struct piece {
	int shape;
	int rot;   /* quarter turns clockwise */
	int x;     /* box position in the well */
	int y;
};

static struct game_view gv;
static struct piece cur;
static bool started;
static int64_t last_tick;

static uint32_t rng;
static uint8_t bag[NUM_SHAPES];
static int bag_left;
static int next_shape;

static int64_t next_fall;
static int64_t lock_at;       /* -1 while not touching down */
static int lock_resets;
static int64_t over_since;

static uint32_t seen[BTN_COUNT];   /* press counters at the last tick */
static int move_dir;               /* -1 left, 1 right, 0 none held */
static int64_t next_repeat;
static int64_t next_soft;

static uint32_t xorshift(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

/* Every shape once per seven, in a random order */
static int draw_shape(void)
{
	if (bag_left == 0) {
		for (int i = 0; i < NUM_SHAPES; i++) {
			bag[i] = i;
		}
		for (int i = NUM_SHAPES - 1; i > 0; i--) {
			int j = xorshift() % (i + 1);
			uint8_t t = bag[i];

			bag[i] = bag[j];
			bag[j] = t;
		}
		bag_left = NUM_SHAPES;
	}
	return bag[--bag_left];
}

/* Block i of a shape turned rot times, in its box */
static void cell_of(int shape, int rot, int i, int *cx, int *cy)
{
	const struct shape *s = &shapes[shape];
	int x = s->cell[i][0];
	int y = s->cell[i][1];

	for (int r = 0; r < (rot & 3); r++) {
		int t = x;

		x = s->n - 1 - y;
		y = t;
	}
	*cx = x;
	*cy = y;
}

/* Room for the piece: inside the walls, above the floor, on free cells */
static bool fits(const struct piece *p)
{
	for (int i = 0; i < 4; i++) {
		int cx, cy;

		cell_of(p->shape, p->rot, i, &cx, &cy);
		cx += p->x;
		cy += p->y;
		if (cx < 0 || cx >= GAME_COLS || cy >= GAME_ROWS) {
			return false;
		}
		if (cy >= 0 && (gv.well[cy] & BIT(cx))) {
			return false;
		}
	}
	return true;
}

static void stamp(uint16_t rows[GAME_ROWS], const struct piece *p)
{
	for (int i = 0; i < 4; i++) {
		int cx, cy;

		cell_of(p->shape, p->rot, i, &cx, &cy);
		cy += p->y;
		if (cy >= 0 && cy < GAME_ROWS) {
			rows[cy] |= BIT(cx + p->x);
		}
	}
}

static uint32_t fall_period(void)
{
	return fall_ms[MIN(gv.level, ARRAY_SIZE(fall_ms) - 1)];
}

/* Next shape in at the top; no room for it ends the game */
static void spawn(int64_t now)
{
	const struct shape *s = &shapes[next_shape];

	cur = (struct piece){
		.shape = next_shape,
		.x = (GAME_COLS - s->n) / 2,
		.y = s->spawn_y,
	};
	next_shape = draw_shape();
	next_fall = now + fall_period();
	lock_at = -1;
	lock_resets = 0;

	if (!fits(&cur)) {
		gv.phase = GAME_OVER;
		over_since = now;
	}
}

static void new_game(int64_t now)
{
	uint32_t best = gv.best;
	uint32_t version = gv.version;

	memset(&gv, 0, sizeof(gv));
	gv.best = best;
	gv.version = version + 1;
	rng ^= k_cycle_get_32();
	if (rng == 0) {
		rng = 1;
	}
	bag_left = 0;
	next_shape = draw_shape();
	spawn(now);
}

static bool on_floor(void)
{
	struct piece down = cur;

	down.y++;
	return !fits(&down);
}

/* A successful move or turn while touching down buys some time */
static void moved(int64_t now)
{
	if (lock_at >= 0 && lock_resets < LOCK_RESETS) {
		lock_resets++;
		lock_at = now + LOCK_MS;
	}
	if (lock_at >= 0 && !on_floor()) {
		lock_at = -1;   /* slid off an edge: falling again */
		next_fall = now + fall_period();
	}
}

static bool shift(int dx, int64_t now)
{
	struct piece p = cur;

	p.x += dx;
	if (!fits(&p)) {
		return false;
	}
	cur = p;
	moved(now);
	return true;
}

static void turn(int64_t now)
{
	ARRAY_FOR_EACH(kicks, k) {
		struct piece p = cur;

		p.rot = (cur.rot + 1) & 3;
		p.x += kicks[k][0];
		p.y += kicks[k][1];
		if (fits(&p)) {
			cur = p;
			moved(now);
			return;
		}
	}
}

/* Settle the piece, clear full rows, score them, bring the next shape */
static void lock(int64_t now)
{
	int cleared = 0;

	stamp(gv.well, &cur);

	/* Settled with a block above the top: no room left */
	for (int i = 0; i < 4; i++) {
		int cx, cy;

		cell_of(cur.shape, cur.rot, i, &cx, &cy);
		if (cur.y + cy < 0) {
			gv.phase = GAME_OVER;
			over_since = now;
			return;
		}
	}

	for (int r = GAME_ROWS - 1; r >= 0;) {
		if (gv.well[r] == BIT_MASK(GAME_COLS)) {
			memmove(&gv.well[1], &gv.well[0], r * sizeof(gv.well[0]));
			gv.well[0] = 0;
			cleared++;
		} else {
			r--;
		}
	}
	if (cleared) {
		gv.score += row_points[cleared] * (gv.level + 1);
		gv.lines += cleared;
		gv.level = gv.lines / LINES_PER_LEVEL;
	}
	spawn(now);
}

/* One row down; touching down starts the lock delay instead */
static bool fall(int64_t now)
{
	if (on_floor()) {
		if (lock_at < 0) {
			lock_at = now + LOCK_MS;
		}
		return false;
	}
	cur.y++;
	return true;
}

static void hard_drop(int64_t now)
{
	while (!on_floor()) {
		cur.y++;
		gv.score += 2;
	}
	lock(now);
}

/* The screen was away: shift every deadline by the time it was */
static void resume(int64_t gap)
{
	next_fall += gap;
	next_soft += gap;
	next_repeat += gap;
	if (lock_at >= 0) {
		lock_at += gap;
	}
	move_dir = 0;
}

/* Presses of a button since the last tick */
static uint32_t presses(const struct inputs_diag *d, enum button b)
{
	uint32_t n = d->count[b] - seen[b];

	seen[b] = d->count[b];
	return n;
}

static void play(const struct inputs_diag *d, int64_t now)
{
	bool left = d->pressed & BIT(BTN_LEFT);
	bool right = d->pressed & BIT(BTN_RIGHT);
	uint32_t n_left = presses(d, BTN_LEFT);
	uint32_t n_right = presses(d, BTN_RIGHT);
	uint32_t n_up = presses(d, BTN_UP);
	uint32_t n_ok = presses(d, BTN_OK);
	uint32_t n_down = presses(d, BTN_DOWN);

	/* Left and right: once per press, then repeating while held */
	for (uint32_t i = 0; i < n_left; i++) {
		shift(-1, now);
	}
	for (uint32_t i = 0; i < n_right; i++) {
		shift(1, now);
	}
	if (n_left || n_right) {
		/* The one pressed last wins while both are held */
		move_dir = n_right ? 1 : -1;
		next_repeat = now + REPEAT_DELAY_MS;
	}
	if ((move_dir < 0 && !left) || (move_dir > 0 && !right)) {
		move_dir = left ? -1 : right ? 1 : 0;
		next_repeat = now + REPEAT_DELAY_MS;
	}
	if (move_dir && now >= next_repeat) {
		shift(move_dir, now);
		next_repeat = now + REPEAT_MS;
	}

	for (uint32_t i = 0; i < n_up; i++) {
		turn(now);
	}
	if (n_ok) {
		hard_drop(now);
		return;
	}

	/* Down: a row per press, then faster while held; a point per row */
	for (uint32_t i = 0; i < n_down; i++) {
		if (fall(now)) {
			gv.score++;
			next_fall = now + fall_period();
		}
		next_soft = now + SOFT_DROP_MS;
	}
	if ((d->pressed & BIT(BTN_DOWN)) && now >= next_soft) {
		if (fall(now)) {
			gv.score++;
			next_fall = now + fall_period();
		}
		next_soft = now + SOFT_DROP_MS;
	}
	if (now >= next_fall) {
		fall(now);
		next_fall = now + fall_period();
	}
	if (lock_at >= 0) {
		if (!on_floor()) {
			lock_at = -1;
		} else if (now >= lock_at) {
			lock(now);
		}
	}
}

/* What the screen shows, from the state above */
static void build_view(void)
{
	struct game_view old = gv;
	struct piece ghost = cur;

	memset(gv.falling, 0, sizeof(gv.falling));
	memset(gv.ghost, 0, sizeof(gv.ghost));
	memset(gv.next, 0, sizeof(gv.next));

	if (gv.phase == GAME_PLAYING) {
		stamp(gv.falling, &cur);
		ghost.y++;
		while (fits(&ghost)) {
			ghost.y++;
		}
		ghost.y--;
		stamp(gv.ghost, &ghost);
	}
	/* Roughly centred in its 4 x 4 box */
	for (int i = 0; i < 4; i++) {
		int n = shapes[next_shape].n;
		int cx, cy;

		cell_of(next_shape, 0, i, &cx, &cy);
		gv.next[cy + (n == 4 ? 0 : 1)] |= BIT(cx + (n == 2 ? 1 : 0));
	}
	gv.best = MAX(gv.best, gv.score);

	old.version = gv.version;
	if (memcmp(&old, &gv, sizeof(gv)) != 0) {
		gv.version++;
	}
}

const struct game_view *game_tick(int64_t now)
{
	struct inputs_diag d;

	inputs_diag(&d);

	if (!started) {
		started = true;
		memcpy(seen, d.count, sizeof(seen));
		new_game(now);
	} else if (now - last_tick > PAUSE_GAP_MS) {
		/* Back on the screen: ignore what was pressed meanwhile */
		memcpy(seen, d.count, sizeof(seen));
		resume(now - last_tick);
	}
	last_tick = now;

	if (gv.phase == GAME_OVER) {
		if (presses(&d, BTN_OK) && now - over_since >= OVER_GUARD_MS) {
			new_game(now);
		}
		memcpy(seen, d.count, sizeof(seen));
	} else {
		play(&d, now);
	}

	build_view();
	return &gv;
}
