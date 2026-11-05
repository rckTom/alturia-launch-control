/*
 * Launch view (FIRING): the inverted screen, showing the pads being fired
 * and a rocket leaving the pad. Hold-down while the exhaust builds, then a
 * constant-acceleration climb off the top of the screen, with the smoke
 * rolling out along the ground and hanging there. It is paced to the
 * second or so the satellites take to confirm the fire pulse; what is left
 * of it when the state ends is simply cut off.
 *
 * The rocket is a stack of rows in its own container, so the whole thing
 * is flown by moving the container; only the exhaust and the smoke are
 * recomputed per frame. Everything is a white box on the black view,
 * which on this panel is the only way to draw a night launch.
 */

#include <zephyr/kernel.h>

#include "views.h"
#include "widgets.h"

#define GROUND_Y    206   /* the ground line */
#define LAUNCH_X    200   /* centre of the pad */
#define MOUNT_H     6     /* the rocket stands this far above the ground */
#define TOWER_H     120

#define ROCKET_W    22    /* body, without the fins */
#define ROCKET_H    94    /* nose tip to nozzle exit */
#define ROCKET_BOX  48    /* body plus both fins */
#define ROCKET_MID  (ROCKET_BOX / 2)
#define NOSE_ROWS   13
#define NOSE_ROW_H  2
#define NOSE_H      (NOSE_ROWS * NOSE_ROW_H)
#define NOZZLE_ROWS 3
#define NOZZLE_H    (NOZZLE_ROWS * NOSE_ROW_H)
#define FIN_TOP     58
#define FIN_ROWS    10
#define FIN_ROW_H   3
#define FLAME_SEGS  8
#define FLAME_SEG_H 6
#define FLAME_W     16
#define SMOKE_PUFFS 12

#define IGNITION_MS   350   /* held down while the exhaust builds */
#define LIFTOFF_ACC   800   /* px/s^2, off the top of the screen in ~0.8 s */
#define SMOKE_STOP_MS 2000  /* after this the cloud just hangs there */
#define PUFF_STEP_MS  30    /* one more puff every */

static lv_obj_t *view;
static lv_obj_t *launch_pads;
static lv_obj_t *rocket;
static lv_obj_t *flame[FLAME_SEGS];
static lv_obj_t *smoke[SMOKE_PUFFS];
static int64_t launch_start = -1;
static uint32_t launch_frame;

/* Ground, launch mount and service tower */
static void create_pad(void)
{
	ui_box(view, 0, GROUND_Y, SCREEN_W, 2);
	ui_box(view, LAUNCH_X - 15, GROUND_Y - MOUNT_H, 30, MOUNT_H);
	ui_box(view, LAUNCH_X - 46, GROUND_Y - TOWER_H, 6, TOWER_H);
	for (int i = 0; i < 3; i++) {
		ui_box(view, LAUNCH_X - 40, GROUND_Y - 30 - i * 30, 18, 3);
	}
}

static void create_rocket(void)
{
	/* Widths of the rows the silhouette is stacked from */
	static const uint8_t nose_w[NOSE_ROWS] = {6, 9, 11, 12, 14, 15, 16, 17, 18, 19,
						  20, 21, 22};
	static const uint8_t nozzle_w[NOZZLE_ROWS] = {12, 14, 16};
	static const uint8_t fin_w[FIN_ROWS] = {1, 2, 4, 5, 7, 8, 10, 11, 12, 13};

	rocket = ui_box(view, 0, 0, ROCKET_BOX, ROCKET_H + FLAME_SEGS * FLAME_SEG_H);
	lv_obj_add_style(rocket, &ui_style_transparent, 0);

	for (int i = 0; i < NOSE_ROWS; i++) {
		ui_box(rocket, ROCKET_MID - nose_w[i] / 2, i * NOSE_ROW_H, nose_w[i], NOSE_ROW_H);
	}
	ui_box(rocket, ROCKET_MID - ROCKET_W / 2, NOSE_H, ROCKET_W, ROCKET_H - NOZZLE_H - NOSE_H);
	for (int i = 0; i < NOZZLE_ROWS; i++) {
		ui_box(rocket, ROCKET_MID - nozzle_w[i] / 2, ROCKET_H - NOZZLE_H + i * NOSE_ROW_H,
		       nozzle_w[i], NOSE_ROW_H);
	}
	for (int i = 0; i < FIN_ROWS; i++) {
		int32_t y = FIN_TOP + i * FIN_ROW_H;

		ui_box(rocket, ROCKET_MID - ROCKET_W / 2 - fin_w[i], y, fin_w[i], FIN_ROW_H);
		ui_box(rocket, ROCKET_MID + ROCKET_W / 2, y, fin_w[i], FIN_ROW_H);
	}
	for (int i = 0; i < FLAME_SEGS; i++) {
		flame[i] = ui_box(rocket, ROCKET_MID, ROCKET_H + i * FLAME_SEG_H, 2, FLAME_SEG_H);
	}
}

void ui_launch_create(lv_obj_t *scr)
{
	lv_obj_t *title;

	view = ui_view(scr, true);

	title = ui_label(view, FONT_LARGE);
	ui_set_text(title, "FIRING");
	lv_obj_set_pos(title, MARGIN, 4);

	launch_pads = ui_text_line(view, FONT_MEDIUM, 240, LV_TEXT_ALIGN_RIGHT);
	lv_obj_align(launch_pads, LV_ALIGN_TOP_RIGHT, -8, 12);

	/* The rocket and its smoke are created last, so they are drawn over the pad */
	create_pad();
	create_rocket();
	for (int i = 0; i < SMOKE_PUFFS; i++) {
		smoke[i] = ui_box(view, 0, 0, 10, 10);
		lv_obj_add_style(smoke[i], &ui_style_round, 0);
	}
}

void ui_launch_reset(void)
{
	launch_start = -1;
}

/* Deterministic noise, so the flame and the smoke are not regular */
static uint32_t jitter(uint32_t n)
{
	n = (n ^ 61) ^ (n >> 16);
	n *= 9;
	n ^= n >> 4;
	n *= 0x27d4eb2d;
	return n ^ (n >> 15);
}

/* Held down while the exhaust builds and the stack shakes, then the climb */
static void fly_rocket(uint32_t frame, int32_t ts)
{
	int32_t x = LAUNCH_X - ROCKET_MID;
	int32_t y = GROUND_Y - MOUNT_H - ROCKET_H;
	int32_t lit = FLAME_SEGS;

	if (ts < IGNITION_MS) {
		x += (int32_t)(jitter(frame) % 3) - 1;
		lit = 1 + FLAME_SEGS * ts / IGNITION_MS;
	} else {
		int32_t dt = ts - IGNITION_MS;   /* ms since release */

		/* h = a t^2 / 2, in two steps so it stays in 32 bits */
		y -= LIFTOFF_ACC * dt / 1000 * dt / 2000;
	}

	ui_set_visible(rocket, y + ROCKET_H + FLAME_SEGS * FLAME_SEG_H >= 0);
	lv_obj_set_pos(rocket, x, y);

	/* Exhaust: tapered segments, each flickering on its own */
	for (int i = 0; i < FLAME_SEGS; i++) {
		int32_t w = FLAME_W * (FLAME_SEGS - i) / FLAME_SEGS +
			    (int32_t)(jitter(frame * 16 + i) % 5) - 2;

		w = MAX(w, 2);
		ui_set_visible(flame[i], i < lit);
		lv_obj_set_pos(flame[i], ROCKET_MID - w / 2, ROCKET_H + i * FLAME_SEG_H);
		lv_obj_set_width(flame[i], w);
	}
}

/* Puffs roll out along the ground, one starting after another */
static void roll_smoke(int32_t ts)
{
	for (int i = 0; i < SMOKE_PUFFS; i++) {
		int32_t dt = MIN(ts, SMOKE_STOP_MS) - i * PUFF_STEP_MS;
		int32_t d, side, rise, x, y;

		ui_set_visible(smoke[i], dt >= 0);
		if (dt < 0) {
			continue;
		}
		d = MIN(12 + dt * 55 / 1000, 58);
		side = (int32_t)(70 + jitter(i) % 110) * dt / 1000;
		rise = (int32_t)(10 + jitter(i * 7 + 2) % 45) * dt / 1000;
		x = LAUNCH_X + ((i & 1) ? side + 6 : -side - 6) + (int32_t)(jitter(i * 3 + 1) % 9) - 4;
		y = GROUND_Y - 4 - rise;
		lv_obj_set_pos(smoke[i], x - d / 2, y - d / 2);
		lv_obj_set_size(smoke[i], d, d);
	}
}

lv_obj_t *ui_launch_refresh(const struct ctrl_view *v, int64_t now)
{
	char buf[PADS_FORMAT_LEN];
	int32_t ts;

	if (launch_start < 0) {
		launch_start = now;
	}
	ts = (int32_t)(now - launch_start);

	pads_format(buf, sizeof(buf), v->selected);
	ui_set_text(launch_pads, buf);

	fly_rocket(launch_frame++, ts);
	roll_smoke(ts);
	return view;
}
