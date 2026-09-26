/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>

#include "link_sched.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef CLAMP
#define CLAMP(v, lo, hi) MIN(MAX(v, lo), hi)
#endif

#define CODING_RATE     1   /* 4/5 */
#define PREAMBLE_SYMS   8

/* xorshift32; nonzero once seeded. */
static uint32_t rng = 0x2545F491u;

void link_sched_seed(uint32_t seed)
{
	rng = seed ? seed : 0x2545F491u;
}

static uint32_t rand_u32(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

/* Uniform in [lo, hi]. */
static uint32_t rand_between(uint32_t lo, uint32_t hi)
{
	return hi <= lo ? lo : lo + rand_u32() % (hi - lo + 1);
}

/*
 * Semtech's time-on-air formula (SX1261/2 datasheet 6.1.4), counted in
 * quarter symbols so it stays in integers.
 */
uint32_t link_airtime_us(size_t len, unsigned int sf, unsigned int bw_khz)
{
	bool ldro = sf >= 11 && bw_khz <= 125;
	int bits = 8 * (int)len - 4 * (int)sf + 16;   /* payload plus CRC */
	int per_block = 4 * ((int)sf - (ldro ? 2 : 0));
	int preamble_q;
	int blocks;
	uint64_t quarters;
	uint64_t us;

	if (sf <= 6) {
		bits += 20;                       /* explicit header */
		preamble_q = 4 * PREAMBLE_SYMS + 25;  /* + 6.25 symbols */
	} else {
		bits += 28;                       /* explicit header + 8 */
		preamble_q = 4 * PREAMBLE_SYMS + 17;  /* + 4.25 symbols */
	}

	blocks = bits > 0 ? (bits + per_block - 1) / per_block : 0;
	quarters = preamble_q + 4 * (8 + blocks * (CODING_RATE + 4));
	us = (quarters * ((uint64_t)1 << sf) * 1000) / (4 * (uint64_t)bw_khz);

	return (uint32_t)us;
}

uint32_t link_airtime_ms(size_t len, unsigned int sf, unsigned int bw_khz)
{
	return (link_airtime_us(len, sf, bw_khz) + 999) / 1000;
}

int link_slot_ms(uint32_t reply_airtime_ms)
{
	uint32_t slot = reply_airtime_ms + (LINK_LBT_LISTEN_US + 999) / 1000 + LINK_SLOT_GUARD_MS;

	return slot > UINT8_MAX ? -EMSGSIZE : (int)slot;
}

void link_util_init(struct link_util *u, int64_t now_us)
{
	memset(u, 0, sizeof(*u));
	u->since_us = now_us;
	u->bucket_start_us = now_us;
}

/* Move the current bucket up to @p t_us, emptying the ones it passes. */
static void util_advance(struct link_util *u, int64_t t_us)
{
	int64_t steps = (t_us - u->bucket_start_us) / LINK_UTIL_BUCKET_US;

	if (steps <= 0) {
		return;
	}
	if (steps >= LINK_UTIL_WINDOW_S) {
		memset(u->busy_us, 0, sizeof(u->busy_us));
	} else {
		for (int64_t i = 0; i < steps; i++) {
			u->cur = (u->cur + 1) % LINK_UTIL_WINDOW_S;
			u->busy_us[u->cur] = 0;
		}
	}
	u->bucket_start_us += steps * LINK_UTIL_BUCKET_US;
}

void link_util_add(struct link_util *u, int64_t start_us, int64_t end_us)
{
	util_advance(u, end_us);

	/* Spread the interval over the buckets it covers, newest first. */
	for (int i = 0; i < LINK_UTIL_WINDOW_S; i++) {
		int64_t lo = u->bucket_start_us - (int64_t)i * LINK_UTIL_BUCKET_US;
		int64_t hi = lo + LINK_UTIL_BUCKET_US;
		int64_t from = start_us > lo ? start_us : lo;
		int64_t to = end_us < hi ? end_us : hi;
		uint32_t *b = &u->busy_us[(u->cur + LINK_UTIL_WINDOW_S - i) % LINK_UTIL_WINDOW_S];

		if (to > from) {
			*b = MIN(*b + (uint32_t)(to - from), LINK_UTIL_BUCKET_US);
		}
		if (lo <= start_us) {
			break;
		}
	}
}

unsigned int link_util_pct(struct link_util *u, int64_t now_us)
{
	int64_t oldest;
	int64_t window;
	uint64_t busy = 0;

	util_advance(u, now_us);

	oldest = u->bucket_start_us - (int64_t)(LINK_UTIL_WINDOW_S - 1) * LINK_UTIL_BUCKET_US;
	if (oldest < u->since_us) {
		oldest = u->since_us;
	}
	window = now_us - oldest;
	/* Too short a view to say anything. */
	if (window < LINK_UTIL_BUCKET_US / 10) {
		return 0;
	}

	for (int i = 0; i < LINK_UTIL_WINDOW_S; i++) {
		busy += u->busy_us[i];
	}

	return (unsigned int)MIN(busy * 100 / (uint64_t)window, 100);
}

const struct link_csma_class link_csma_ctrl = {
	.cw_min = LINK_CSMA_CTRL_CW_MIN,
	.cw_max = LINK_CSMA_CTRL_CW_MAX,
	.max_defer_us = LINK_LBT_MAX_DEFER_MS * 1000,
	.force = true,
};

const struct link_csma_class link_csma_slot = {
	.cw_min = 0,
	.cw_max = 0,
	.max_defer_us = 0,
	.force = false,
};

/* The controller leaves 10 ms for the answer to start (ANSWER_DEFER_MS). */
const struct link_csma_class link_csma_answer = {
	.cw_min = 0,
	.cw_max = 1,
	.max_defer_us = 10000 - LINK_CSMA_SLOT_US,
	.force = false,
};

const struct link_csma_class link_csma_unsolicited = {
	.cw_min = LINK_CSMA_SAT_CW_MIN,
	.cw_max = LINK_CSMA_SAT_CW_MAX,
	.max_defer_us = 200000,
	.force = false,
};

uint8_t link_csma_cw(const struct link_csma_class *c, unsigned int util_pct)
{
	unsigned int span = c->cw_max > c->cw_min ? c->cw_max - c->cw_min : 0;

	/* Linear in utilization, rounded to the nearest step, as in Meshtastic. */
	return c->cw_min + (MIN(util_pct, 100) * span + 50) / 100;
}

/* A random backoff of 0 .. 2^cw - 1 slots. */
static int64_t csma_backoff_us(uint8_t cw)
{
	return (int64_t)rand_between(0, (1u << cw) - 1) * LINK_CSMA_SLOT_US;
}

int64_t link_csma_begin(struct link_csma *s, const struct link_csma_class *c,
			unsigned int util_pct, int64_t now_us)
{
	s->c = c;
	s->cw = link_csma_cw(c, util_pct);
	s->was_busy = false;
	s->deadline_us = now_us + (int64_t)((1u << s->cw) - 1) * LINK_CSMA_SLOT_US + c->max_defer_us;

	return now_us + csma_backoff_us(s->cw);
}

enum link_csma_res link_csma_step(struct link_csma *s, bool clear, int64_t now_us,
				  int64_t *next_us)
{
	int64_t next;

	if (clear) {
		if (!s->was_busy || now_us >= s->deadline_us) {
			return LINK_CSMA_SEND;
		}
		/* It has just cleared: everyone who waited senses it now, so draw again. */
		s->was_busy = false;
		next = now_us + csma_backoff_us(s->cw);
	} else {
		if (now_us >= s->deadline_us) {
			return s->c->force ? LINK_CSMA_FORCED : LINK_CSMA_GIVE_UP;
		}
		/* Wait for the channel to clear, sensing every slot. */
		s->was_busy = true;
		next = now_us + LINK_CSMA_SLOT_US;
	}

	/* Always get one last sense in at the deadline. */
	*next_us = next < s->deadline_us ? next : s->deadline_us;
	return LINK_CSMA_SENSE;
}

void link_foreign_init(struct link_foreign *f)
{
	memset(f, 0, sizeof(*f));
}

static bool foreign_live(const struct link_foreign_net *n, int64_t now_us)
{
	return n->used && now_us - n->heard_us < LINK_FOREIGN_FORGET_US;
}

int link_foreign_count(const struct link_foreign *f, int64_t now_us)
{
	int n = 0;

	for (int i = 0; i < LINK_FOREIGN_NETS; i++) {
		n += foreign_live(&f->net[i], now_us);
	}
	return n;
}

static struct link_foreign_net *foreign_find(struct link_foreign *f, uint16_t tag, int64_t now_us)
{
	struct link_foreign_net *free = NULL;

	for (int i = 0; i < LINK_FOREIGN_NETS; i++) {
		struct link_foreign_net *n = &f->net[i];

		if (n->used && n->tag == tag) {
			return n;
		}
		/* An empty entry, else the one heard longest ago. */
		if (!free || (free->used && (!n->used || n->heard_us < free->heard_us))) {
			free = n;
		}
	}

	*free = (struct link_foreign_net){
		.used = true,
		.tag = tag,
		.heard_us = now_us,
	};
	return free;
}

void link_foreign_heard(struct link_foreign *f, uint16_t tag, bool poll, bool from_ctrl,
			int64_t start_us, int64_t end_us, uint32_t answer_us)
{
	struct link_foreign_net *n = foreign_find(f, tag, end_us);

	if (!foreign_live(n, end_us)) {
		/* Back after a silence: what we knew is stale. */
		*n = (struct link_foreign_net){
			.used = true,
			.tag = tag,
		};
	}
	n->heard_us = end_us;

	if (poll) {
		if (n->poll_us) {
			int64_t gap = start_us - n->poll_us;
			/* The cycle that just ended, as far as we heard its replies. */
			int64_t seen = n->cycle_end_us - n->poll_us;

			if (gap >= 200000 && gap <= 5000000) {
				n->period_us = n->period_us ? (3 * n->period_us + gap) / 4 : gap;
			}
			/*
			 * The first cycle seen replaces the guess. After that,
			 * grow at once and shrink slowly: a satellite that missed
			 * one poll leaves a silent slot, but it is still there.
			 */
			if (!n->cycle_us || seen > n->cycle_us) {
				n->cycle_us = seen;
			} else {
				n->cycle_us -= n->cycle_us / 8;
			}
			if (n->cycle_us < end_us - start_us) {
				n->cycle_us = end_us - start_us;
			}
		}
		n->poll_us = start_us;
		n->cycle_end_us = end_us;
		return;
	}

	if (!from_ctrl && n->poll_us && start_us - n->poll_us < LINK_FOREIGN_CYCLE_MAX_US &&
	    end_us > n->cycle_end_us) {
		n->cycle_end_us = end_us;
	}
	if (from_ctrl && answer_us && end_us + answer_us > n->reserved_until_us) {
		n->reserved_until_us = end_us + answer_us;
	}
}

/*
 * The cycle length we reserve for network @p n: what we heard of it, but
 * never less than our own. Its satellites may be too far away for us to
 * hear their replies, and it runs the same firmware as we do.
 */
static int64_t foreign_cycle_len(const struct link_foreign *f, const struct link_foreign_net *n)
{
	int64_t len = n->cycle_us ? n->cycle_us : LINK_FOREIGN_CYCLE_US;

	len = MAX(len, n->cycle_end_us - n->poll_us);
	return MAX(len, (int64_t)f->min_cycle_us);
}

void link_foreign_min_cycle(struct link_foreign *f, uint32_t cycle_us)
{
	f->min_cycle_us = cycle_us;
}

/* If [t, t + need) meets [from, to), the end of that window; else t. */
static int64_t foreign_clear(int64_t t, int64_t need, int64_t from, int64_t to)
{
	return t < to && from < t + need ? to : t;
}

int64_t link_foreign_start(const struct link_foreign *f, int64_t now_us, uint32_t need_us,
			   uint32_t max_wait_us)
{
	int64_t t = now_us;

	/* Each pass moves t past a window; stop when one pass moves nothing. */
	for (int pass = 0; pass < 4 * LINK_FOREIGN_NETS + 1; pass++) {
		int64_t before = t;

		for (int i = 0; i < LINK_FOREIGN_NETS; i++) {
			const struct link_foreign_net *n = &f->net[i];

			if (!foreign_live(n, now_us)) {
				continue;
			}
			t = foreign_clear(t, need_us, now_us, n->reserved_until_us);
			if (!n->poll_us) {
				continue;
			}

			int64_t len = foreign_cycle_len(f, n);

			t = foreign_clear(t, need_us, n->poll_us, n->poll_us + len);
			for (int k = 1; n->period_us && k <= 2; k++) {
				int64_t at = n->poll_us + k * n->period_us;

				t = foreign_clear(t, need_us, at - LINK_FOREIGN_MARGIN_US,
						  at + len + LINK_FOREIGN_MARGIN_US);
			}
		}

		if (t - now_us > max_wait_us) {
			return now_us;
		}
		if (t == before) {
			return t;
		}
	}
	return now_us;
}

int32_t link_foreign_shift_us(const struct link_foreign *f, int64_t now_us, int64_t our_start_us,
			      uint32_t our_len_us, uint32_t period_us)
{
	const int64_t p = period_us;
	/* Cycle starts relative to ours, in [0, p): the last before our next, the first after. */
	int64_t prev_start = -1;
	int64_t prev_end = 0;
	int64_t next_start = -1;
	int64_t lo;
	int64_t hi;
	int64_t shift;

	for (int i = 0; i < LINK_FOREIGN_NETS; i++) {
		const struct link_foreign_net *n = &f->net[i];
		int64_t phase;

		if (!foreign_live(n, now_us) || !n->poll_us || !n->period_us ||
		    n->period_us * 10 < p * 8 || n->period_us * 10 > p * 12) {
			continue;
		}

		phase = (n->poll_us - our_start_us) % p;
		if (phase < 0) {
			phase += p;
		}
		if (phase > prev_start) {
			prev_start = phase;
			prev_end = phase + foreign_cycle_len(f, n);
		}
		if (next_start < 0 || phase < next_start) {
			next_start = phase;
		}
	}

	if (prev_start < 0) {
		return 0;
	}

	/*
	 * Our next cycle may start anywhere after the previous network's cycle
	 * has ended and must end before the next one's starts (one period on).
	 */
	lo = prev_end + LINK_FOREIGN_MARGIN_US;
	hi = next_start + p - LINK_FOREIGN_MARGIN_US - our_len_us;
	if (hi < lo) {
		return 0;
	}

	shift = ((lo + hi) / 2 - p) * LINK_DESYNC_ALPHA_PCT / 100;
	return (int32_t)CLAMP(shift, -(int64_t)LINK_DESYNC_EARLY_MAX_US,
			      (int64_t)LINK_DESYNC_LATE_MAX_US);
}
