/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * link_sched: airtime, reply slots, channel utilization, the CSMA/CA
 * policy and the estimate of other networks' air time.
 */

#include <zephyr/ztest.h>

#include "link_sched.h"
#include "lsec.h"

#define US_PER_MS 1000

static void assert_near(uint32_t got, uint32_t want, uint32_t tol, const char *what)
{
	zassert_true(got + tol >= want && got <= want + tol, "%s: %u ms, expected %u", what, got,
		     want);
}

ZTEST(link_sched, test_airtime_formula)
{
	/* Semtech's calculator for SF7, 250 kHz, CR 4/5, preamble 8, CRC on. */
	assert_near(link_airtime_ms(22, 7, 250), 28, 1, "22 B");
	assert_near(link_airtime_ms(50, 7, 250), 49, 1, "50 B");
	assert_near(link_airtime_ms(63, 7, 250), 59, 1, "63 B");
	/* The old link settings, and SF5 with its longer preamble term. */
	assert_near(link_airtime_ms(22, 9, 125), 206, 1, "SF9 22 B");
	assert_near(link_airtime_ms(22, 5, 250), 9, 1, "SF5 22 B");
	/* Low data rate optimisation kicks in at SF11 and 125 kHz. */
	assert_near(link_airtime_ms(22, 12, 125), 1483, 1, "SF12 22 B");
}

ZTEST(link_sched, test_slots_do_not_overlap)
{
	for (size_t len = 0; len <= LSEC_MAX_PAYLOAD; len++) {
		uint32_t air = link_airtime_ms(len + LSEC_OVERHEAD, 7, 250);
		int slot = link_slot_ms(air);

		if (slot < 0) {
			zassert_true(air + LINK_SLOT_GUARD_MS > UINT8_MAX - 1, "%zu B refused", len);
			continue;
		}

		for (uint8_t rank = 0; rank < LSEC_MAX_SATS; rank++) {
			int64_t start = link_slot_start_us(0, rank, slot);
			int64_t next = link_slot_start_us(0, rank + 1, slot);

			zassert_true(start >= LINK_TURNAROUND_MS * US_PER_MS);
			/* Listen, transmit, and still leave the guard before the next. */
			zassert_true(start + LINK_LBT_LISTEN_US + air * US_PER_MS +
					     LINK_SLOT_GUARD_MS * US_PER_MS <= next,
				     "%zu B overlaps at rank %u", len, rank);
		}
		zassert_equal(link_replies_end_us(0, 2, slot), link_slot_start_us(0, 2, slot));
	}
}

/* ---- CSMA/CA ------------------------------------------------------- */

static const struct link_csma_class test_class = {
	.cw_min = 3,
	.cw_max = 7,
	.max_defer_us = 50 * US_PER_MS,
	.force = true,
};

ZTEST(link_sched, test_csma_cw_follows_utilization)
{
	zassert_equal(link_csma_cw(&test_class, 0), 3);
	zassert_equal(link_csma_cw(&test_class, 50), 5);
	zassert_equal(link_csma_cw(&test_class, 100), 7);
	zassert_equal(link_csma_cw(&test_class, 250), 7, "utilization above 100 %%");
	for (unsigned int u = 1; u <= 100; u++) {
		zassert_true(link_csma_cw(&test_class, u) >= link_csma_cw(&test_class, u - 1));
	}
}

ZTEST(link_sched, test_csma_backoff_in_window)
{
	struct link_csma s;

	link_sched_seed(7);
	for (unsigned int u = 0; u <= 100; u += 25) {
		uint8_t cw = link_csma_cw(&test_class, u);
		int64_t max = 0;

		for (int i = 0; i < 500; i++) {
			int64_t first = link_csma_begin(&s, &test_class, u, 1000);

			zassert_true(first >= 1000, "sense before the start");
			zassert_true(first - 1000 <= (int64_t)((1 << cw) - 1) * LINK_CSMA_SLOT_US,
				     "backoff outside the window at %u %%", u);
			zassert_equal((first - 1000) % LINK_CSMA_SLOT_US, 0, "not whole slots");
			max = MAX(max, first - 1000);
		}
		/* 500 draws out of at most 128 slots reach the top half. */
		zassert_true(max >= (int64_t)((1 << cw) / 2) * LINK_CSMA_SLOT_US,
			     "window at %u %% not used", u);
	}
}

ZTEST(link_sched, test_csma_clear_sends)
{
	struct link_csma s;
	int64_t next = -1;
	int64_t at = link_csma_begin(&s, &test_class, 0, 5000);

	zassert_equal(link_csma_step(&s, true, at, &next), LINK_CSMA_SEND);
	zassert_equal(next, -1, "next sense written on a clear channel");
}

ZTEST(link_sched, test_csma_busy_then_redraw)
{
	struct link_csma s;
	int64_t now = link_csma_begin(&s, &test_class, 0, 0);
	int64_t next;

	/* Busy: sense again one slot later, until it clears. */
	for (int i = 0; i < 5; i++) {
		zassert_equal(link_csma_step(&s, false, now, &next), LINK_CSMA_SENSE);
		zassert_equal(next, now + LINK_CSMA_SLOT_US);
		now = next;
	}

	/* Cleared: not at once, but after a fresh backoff and one more sense. */
	link_sched_seed(3);
	bool waited = false;

	for (int i = 0; i < 20 && !waited; i++) {
		struct link_csma t = s;

		zassert_equal(link_csma_step(&t, true, now, &next), LINK_CSMA_SENSE,
			      "sent on the first clear sense after a busy one");
		zassert_true(next >= now && next <= now + ((1 << 3) - 1) * LINK_CSMA_SLOT_US);
		waited |= next > now;
		zassert_equal(link_csma_step(&t, true, next, &next), LINK_CSMA_SEND);
	}
	zassert_true(waited, "no backoff drawn after the channel cleared");
}

static enum link_csma_res busy_until_done(bool force, int64_t *end, int64_t *deadline)
{
	const struct link_csma_class c = {
		.cw_min = 3,
		.cw_max = 3,
		.max_defer_us = 50 * US_PER_MS,
		.force = force,
	};
	struct link_csma s;
	int64_t now = link_csma_begin(&s, &c, 0, 0);
	enum link_csma_res res;
	int64_t next;

	*deadline = s.deadline_us;
	for (int i = 0; i < 1000; i++) {
		res = link_csma_step(&s, false, now, &next);
		if (res != LINK_CSMA_SENSE) {
			*end = now;
			return res;
		}
		zassert_true(next <= s.deadline_us, "sense scheduled past the deadline");
		now = next;
	}

	ztest_test_fail();
	return res;
}

ZTEST(link_sched, test_csma_deadline)
{
	int64_t end;
	int64_t deadline;

	/* The controller sends anyway, exactly at the deadline and not later. */
	zassert_equal(busy_until_done(true, &end, &deadline), LINK_CSMA_FORCED);
	zassert_equal(end, deadline);
	zassert_equal(deadline, 7 * LINK_CSMA_SLOT_US + 50 * US_PER_MS,
		      "deadline is the window plus max_defer");

	/* A satellite gives up instead. */
	zassert_equal(busy_until_done(false, &end, &deadline), LINK_CSMA_GIVE_UP);
	zassert_equal(end, deadline);
}

ZTEST(link_sched, test_csma_slot_single_sense)
{
	struct link_csma s;
	int64_t next;

	/* A reply slot: sense at its start, then send or skip, never wait. */
	zassert_equal(link_csma_begin(&s, &link_csma_slot, 100, 9000), 9000);
	zassert_equal(link_csma_step(&s, false, 9000 + LINK_LBT_LISTEN_US, &next),
		      LINK_CSMA_GIVE_UP);
	link_csma_begin(&s, &link_csma_slot, 100, 9000);
	zassert_equal(link_csma_step(&s, true, 9000 + LINK_LBT_LISTEN_US, &next), LINK_CSMA_SEND);
}

ZTEST(link_sched, test_csma_answer_fits_gap)
{
	struct link_csma s;

	/* Answers must start within the 10 ms the controller leaves for them. */
	link_csma_begin(&s, &link_csma_answer, 100, 0);
	zassert_true(s.deadline_us <= 10 * US_PER_MS, "answer deadline %lld us", s.deadline_us);
}

ZTEST(link_sched, test_csma_seeds_differ)
{
	struct link_csma s;
	int64_t a[8];
	int64_t b[8];
	bool differ = false;

	/* Two boards seeded differently draw different backoffs. */
	link_sched_seed(1);
	for (int i = 0; i < ARRAY_SIZE(a); i++) {
		a[i] = link_csma_begin(&s, &test_class, 100, 0);
	}
	link_sched_seed(2);
	for (int i = 0; i < ARRAY_SIZE(b); i++) {
		b[i] = link_csma_begin(&s, &test_class, 100, 0);
		differ |= a[i] != b[i];
	}
	zassert_true(differ);
}

/* ---- channel utilization ------------------------------------------- */

#define SEC(x)      ((int64_t)(x) * 1000000)
#define MS_TO_US(x) ((int64_t)(x) * US_PER_MS)

ZTEST(link_sched, test_util_share)
{
	struct link_util u;

	link_util_init(&u, 0);
	zassert_equal(link_util_pct(&u, 0), 0);

	/* 200 ms of every 1.2 s: about 17 %. */
	for (int64_t t = 0; t < SEC(12); t += MS_TO_US(1200)) {
		link_util_add(&u, t, t + MS_TO_US(200));
	}
	zassert_within(link_util_pct(&u, SEC(12)), 16, 2);
}

ZTEST(link_sched, test_util_ages_out)
{
	struct link_util u;

	link_util_init(&u, 0);
	link_util_add(&u, 0, SEC(3));
	zassert_equal(link_util_pct(&u, SEC(3)), 100);
	zassert_within(link_util_pct(&u, SEC(6)), 50, 1);
	/* The busy spell has left the window. */
	zassert_equal(link_util_pct(&u, SEC(3) + SEC(LINK_UTIL_WINDOW_S) + SEC(1)), 0);
	/* And a long silence skips every bucket. */
	zassert_equal(link_util_pct(&u, SEC(1000)), 0);
}

ZTEST(link_sched, test_util_spans_buckets_and_caps)
{
	struct link_util u;

	link_util_init(&u, 0);
	/* One interval across three buckets counts fully. */
	link_util_add(&u, MS_TO_US(500), MS_TO_US(2500));
	zassert_within(link_util_pct(&u, SEC(4)), 50, 1);

	/* Overlapping intervals cannot push it above 100 %. */
	link_util_init(&u, 0);
	for (int i = 0; i < 5; i++) {
		link_util_add(&u, 0, SEC(2));
	}
	zassert_equal(link_util_pct(&u, SEC(2)), 100);
}

/* ---- other networks ------------------------------------------------ */

#define MS(x)    ((int64_t)(x) * US_PER_MS)
#define TAG      0x1234
#define POLL_AIR MS(31)

/*
 * Another network: a POLL every period_ms starting at t0, and two replies
 * of 36 ms in 66 ms slots after a 6 ms turnaround. Returns the last POLL.
 */
static int64_t other_network(struct link_foreign *f, int64_t t0, int polls, int period_ms)
{
	int64_t p = t0;

	for (int i = 0; i < polls; i++) {
		p = t0 + i * MS(period_ms);
		link_foreign_heard(f, TAG, true, true, p, p + POLL_AIR, 0);
		for (int k = 0; k < 2; k++) {
			int64_t r = p + POLL_AIR + MS(6) + k * MS(66) + MS(1);

			link_foreign_heard(f, TAG, false, false, r, r + MS(36), 0);
		}
	}
	return p;
}

ZTEST(link_sched, test_foreign_learns_and_avoids)
{
	struct link_foreign f;
	int64_t last;
	int64_t cycle_end;
	int64_t t;

	link_foreign_init(&f);
	last = other_network(&f, MS(1000), 4, 1200);
	cycle_end = last + POLL_AIR + MS(6) + MS(66) + MS(1) + MS(36);

	zassert_equal(f.net[0].period_us, MS(1200));
	zassert_equal(f.net[0].cycle_us, cycle_end - last);

	/* Inside the cycle: wait for its end. */
	t = link_foreign_start(&f, last + MS(50), MS(40), MS(400));
	zassert_equal(t, cycle_end);

	/* In the gap, with room: at once. */
	t = link_foreign_start(&f, last + MS(500), MS(40), MS(400));
	zassert_equal(t, last + MS(500));

	/* Just before the next predicted POLL, without room: after that cycle. */
	t = link_foreign_start(&f, last + MS(1150), MS(80), MS(400));
	zassert_equal(t, cycle_end + MS(1200) + LINK_FOREIGN_MARGIN_US);
}

ZTEST(link_sched, test_foreign_wait_is_capped)
{
	struct link_foreign f;
	int64_t last;

	link_foreign_init(&f);
	last = other_network(&f, MS(1000), 3, 1200);

	/* The cycle ends 140 ms on; with a 100 ms cap, go at once. */
	zassert_equal(link_foreign_start(&f, last + MS(10), MS(40), MS(100)), last + MS(10));
}

ZTEST(link_sched, test_foreign_answer_window)
{
	struct link_foreign f;
	int64_t t0 = MS(5000);

	link_foreign_init(&f);
	/* Another controller's JOIN_OK, answered within 60 ms of its end. */
	link_foreign_heard(&f, TAG, false, true, t0, t0 + MS(41), MS(60));

	zassert_equal(link_foreign_start(&f, t0 + MS(45), MS(20), MS(400)), t0 + MS(101));
	zassert_equal(link_foreign_start(&f, t0 + MS(120), MS(20), MS(400)), t0 + MS(120));
}

ZTEST(link_sched, test_foreign_forgotten)
{
	struct link_foreign f;
	int64_t last;

	link_foreign_init(&f);
	last = other_network(&f, MS(1000), 3, 1200);

	/* Silent for longer than LINK_FOREIGN_FORGET_US: no longer in the way. */
	zassert_equal(link_foreign_start(&f, last + LINK_FOREIGN_FORGET_US + MS(1), MS(40), MS(400)),
		      last + LINK_FOREIGN_FORGET_US + MS(1));
}

ZTEST(link_sched, test_foreign_unheard_replies_reserve_our_cycle)
{
	struct link_foreign f;
	int64_t t = MS(5000);

	/* We hear the other controller's polls, never its satellites. */
	link_foreign_init(&f);
	for (int i = 0; i < 3; i++, t += MS(1200)) {
		link_foreign_heard(&f, TAG, true, true, t, t + POLL_AIR, 0);
	}
	t -= MS(1200);

	/* Alone, a frame right after its poll looks fine... */
	zassert_equal(link_foreign_start(&f, t + POLL_AIR + MS(1), MS(40), MS(400)),
		      t + POLL_AIR + MS(1));

	/* ...but its replies are there all the same: as long as our own cycle. */
	link_foreign_min_cycle(&f, MS(130));
	zassert_true(link_foreign_start(&f, t + POLL_AIR + MS(1), MS(40), MS(400)) >= t + MS(130),
		     "sent into the other network's reply slots");
}

/* ---- poll phase, DESYNC ------------------------------------------- */

/* A CSMA backoff out of 2^cw slots, from the link's own generator. */
static int64_t rand_backoff_us(uint8_t cw)
{
	const struct link_csma_class c = {.cw_min = cw, .cw_max = cw};
	struct link_csma s;

	return link_csma_begin(&s, &c, 0, 0);
}

#define PERIOD   MS(1200)
#define CYCLE    MS(200)

/* Network @p tag polled at @p start and its satellites answered until start + CYCLE. */
static void heard_cycle(struct link_foreign *f, uint16_t tag, int64_t start)
{
	link_foreign_heard(f, tag, true, true, start, start + POLL_AIR, 0);
	link_foreign_heard(f, tag, false, false, start + CYCLE - MS(36), start + CYCLE, 0);
}

static int64_t phase_of(int64_t t)
{
	return ((t % PERIOD) + PERIOD) % PERIOD;
}

ZTEST(link_sched, test_desync_alone_or_fast)
{
	struct link_foreign f;

	link_foreign_init(&f);
	zassert_equal(link_foreign_shift_us(&f, MS(10000), MS(10000), CYCLE, PERIOD), 0,
		      "moved with nobody around");

	/* A network repeating a command every 300 ms is left to link_foreign_start(). */
	for (int i = 0; i < 10; i++) {
		heard_cycle(&f, TAG, MS(7000) + i * MS(300));
	}
	zassert_equal(link_foreign_shift_us(&f, MS(10000), MS(10000), CYCLE, PERIOD), 0);
}

ZTEST(link_sched, test_desync_no_room)
{
	struct link_foreign f;
	int64_t t = MS(5000);

	link_foreign_init(&f);
	/* A neighbour whose cycle takes 950 ms of the 1.2 s: ours (300 ms) cannot fit. */
	for (int i = 0; i < 4; i++, t += PERIOD) {
		link_foreign_heard(&f, TAG, true, true, t, t + POLL_AIR, 0);
		link_foreign_heard(&f, TAG, false, false, t + MS(900), t + MS(950), 0);
	}
	zassert_equal(link_foreign_shift_us(&f, t, t - MS(600), MS(300), PERIOD), 0);
	/* A shorter cycle of ours would. */
	zassert_not_equal(link_foreign_shift_us(&f, t, t - MS(600), MS(150), PERIOD), 0);
}

ZTEST(link_sched, test_desync_one_neighbour)
{
	struct link_foreign f;
	int64_t theirs = MS(5000);
	int64_t ours = MS(5010);   /* in step with them: the worst case */
	int32_t shift;

	link_foreign_init(&f);
	/* Their period is known from two polls. */
	heard_cycle(&f, TAG, theirs - PERIOD);
	for (int round = 0; round < 12; round++) {
		heard_cycle(&f, TAG, theirs);
		shift = link_foreign_shift_us(&f, ours + CYCLE, ours, CYCLE, PERIOD);
		zassert_true(shift >= -LINK_DESYNC_EARLY_MAX_US && shift <= LINK_DESYNC_LATE_MAX_US);
		if (round == 0) {
			zassert_not_equal(shift, 0, "did not move away from a neighbour in step");
		}
		ours += PERIOD + shift;
		theirs += PERIOD;
	}

	/* Half a period apart, give or take a few ms. */
	int64_t apart = phase_of(ours - theirs);

	zassert_within(apart, PERIOD / 2, MS(10), "%lld us apart", apart);
	zassert_within(link_foreign_shift_us(&f, ours, ours - PERIOD, CYCLE, PERIOD), 0, MS(5),
		       "still moving once settled");
}

ZTEST(link_sched, test_desync_four_networks_spread)
{
	enum { NETS = 4 };
	struct link_foreign f[NETS];
	int64_t start[NETS];

	link_sched_seed(11);
	for (int i = 0; i < NETS; i++) {
		link_foreign_init(&f[i]);
		start[i] = MS(5000) + i * MS(5);   /* all in step */
	}

	for (int round = 0; round < 40; round++) {
		int32_t shift[NETS];

		/* Everyone hears everyone else's cycle of this round. */
		for (int i = 0; i < NETS; i++) {
			for (int j = 0; j < NETS; j++) {
				if (j != i) {
					heard_cycle(&f[i], TAG + j, start[j]);
				}
			}
		}
		for (int i = 0; i < NETS; i++) {
			shift[i] = link_foreign_shift_us(&f[i], start[i] + PERIOD / 2, start[i], CYCLE,
							 PERIOD);
		}
		/* The next polls, each after its own random CSMA backoff. */
		for (int i = 0; i < NETS; i++) {
			start[i] += PERIOD + shift[i] + rand_backoff_us(7);
		}
	}

	/* Every cycle ends, with the margin, before the next one starts. */
	int64_t ph[NETS];

	for (int i = 0; i < NETS; i++) {
		ph[i] = phase_of(start[i]);
	}
	for (int i = 0; i < NETS; i++) {
		int64_t gap = PERIOD;

		for (int j = 0; j < NETS; j++) {
			if (j != i) {
				gap = MIN(gap, phase_of(ph[j] - ph[i]));
			}
		}
		zassert_true(gap >= CYCLE + LINK_FOREIGN_MARGIN_US,
			     "net %d: next cycle only %lld ms later", i, gap / 1000);
	}
}

ZTEST_SUITE(link_sched, NULL, NULL, NULL, NULL, NULL);
