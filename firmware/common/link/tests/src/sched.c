/* SPDX-License-Identifier: Apache-2.0 */

/* link_sched: airtime, reply slots and the listen-before-talk policy. */

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

ZTEST(link_sched, test_lbt_clear_first_try)
{
	struct link_lbt l;
	int64_t retry = -1;

	zassert_equal(link_lbt_begin(&l, 5000, 50 * US_PER_MS, true, false), 5000);
	zassert_equal(link_lbt_step(&l, true, 5000, &retry), LINK_LBT_SEND);
	zassert_equal(retry, -1, "retry time written on a clear channel");
}

ZTEST(link_sched, test_lbt_backoff_bounds)
{
	struct link_lbt l;
	int64_t now = 0;
	int64_t retry;

	link_lbt_begin(&l, now, 1000 * US_PER_MS, true, false);

	for (int i = 0; i < 200; i++) {
		zassert_equal(link_lbt_step(&l, false, now, &retry), LINK_LBT_RETRY);
		zassert_true(retry >= now + US_PER_MS, "backoff under 1 ms");
		zassert_true(retry <= now + LINK_LBT_BACKOFF_MAX_MS * US_PER_MS, "backoff too long");
		now = retry;
		if (now >= l.deadline_us - LINK_LBT_BACKOFF_MAX_MS * US_PER_MS) {
			break;
		}
	}
}

static enum link_lbt_res busy_until_done(bool force, int64_t *end)
{
	struct link_lbt l;
	int64_t now = link_lbt_begin(&l, 0, LINK_LBT_MAX_DEFER_MS * US_PER_MS, force, false);
	enum link_lbt_res res;
	int64_t retry;

	for (int i = 0; i < 1000; i++) {
		res = link_lbt_step(&l, false, now, &retry);
		if (res != LINK_LBT_RETRY) {
			*end = now;
			return res;
		}
		zassert_true(retry <= l.deadline_us, "listen scheduled past the deadline");
		now = retry;
	}

	ztest_test_fail();
	return res;
}

ZTEST(link_sched, test_lbt_cap)
{
	int64_t end;

	/* The controller sends anyway, exactly at the cap and not later. */
	zassert_equal(busy_until_done(true, &end), LINK_LBT_FORCED);
	zassert_equal(end, LINK_LBT_MAX_DEFER_MS * US_PER_MS);

	/* A satellite gives up instead. */
	zassert_equal(busy_until_done(false, &end), LINK_LBT_GIVE_UP);
	zassert_equal(end, LINK_LBT_MAX_DEFER_MS * US_PER_MS);
}

ZTEST(link_sched, test_lbt_predelay_random)
{
	struct link_lbt l;
	int64_t first[8];
	bool differ = false;

	link_sched_seed(1);
	for (int i = 0; i < ARRAY_SIZE(first); i++) {
		first[i] = link_lbt_begin(&l, 0, 50 * US_PER_MS, false, true);
		zassert_true(first[i] >= 0 && first[i] <= LINK_LBT_BACKOFF_MAX_MS * US_PER_MS);
		differ |= first[i] != first[0];
	}
	zassert_true(differ, "pre-delay is not random");

	/* Two boards seeded differently pick different delays. */
	link_sched_seed(1);
	int64_t a = link_lbt_begin(&l, 0, 50 * US_PER_MS, false, true);

	link_sched_seed(2);
	zassert_not_equal(link_lbt_begin(&l, 0, 50 * US_PER_MS, false, true), a);
}

ZTEST_SUITE(link_sched, NULL, NULL, NULL, NULL, NULL);
