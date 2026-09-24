/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include "link_sched.h"

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
uint32_t link_airtime_ms(size_t len, unsigned int sf, unsigned int bw_khz)
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

	return (uint32_t)((us + 999) / 1000);
}

int link_slot_ms(uint32_t reply_airtime_ms)
{
	uint32_t slot = reply_airtime_ms + (LINK_LBT_LISTEN_US + 999) / 1000 + LINK_SLOT_GUARD_MS;

	return slot > UINT8_MAX ? -EMSGSIZE : (int)slot;
}

int64_t link_lbt_begin(struct link_lbt *l, int64_t now_us, uint32_t max_defer_us, bool force,
		       bool predelay)
{
	l->deadline_us = now_us + max_defer_us;
	l->force = force;

	if (!predelay) {
		return now_us;
	}

	return now_us + rand_between(0, LINK_LBT_BACKOFF_MAX_MS * 1000);
}

enum link_lbt_res link_lbt_step(struct link_lbt *l, bool clear, int64_t now_us,
				int64_t *retry_at_us)
{
	int64_t retry;

	if (clear) {
		return LINK_LBT_SEND;
	}

	if (now_us >= l->deadline_us) {
		return l->force ? LINK_LBT_FORCED : LINK_LBT_GIVE_UP;
	}

	/* Back off, but always get one last listen in at the deadline. */
	retry = now_us + rand_between(1000, LINK_LBT_BACKOFF_MAX_MS * 1000);
	*retry_at_us = retry < l->deadline_us ? retry : l->deadline_us;

	return LINK_LBT_RETRY;
}
