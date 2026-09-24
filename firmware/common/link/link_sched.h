/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Channel access timing, shared by sat_link, ctrl_link, the controller's
 * satellite simulator and the unit tests: airtime, reply slots and the
 * listen-before-talk policy. Pure: time comes in as an argument, in
 * microseconds. See design/radio_security.md, "Channel access".
 */

#ifndef LINK_SCHED_H_
#define LINK_SCHED_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The ../link/Kconfig values, with the same defaults outside an app. */
#ifdef CONFIG_LINK_TURNAROUND_MS
#define LINK_TURNAROUND_MS CONFIG_LINK_TURNAROUND_MS
#else
#define LINK_TURNAROUND_MS 6
#endif
#ifdef CONFIG_LINK_SLOT_GUARD_MS
#define LINK_SLOT_GUARD_MS CONFIG_LINK_SLOT_GUARD_MS
#else
#define LINK_SLOT_GUARD_MS 6
#endif
#ifdef CONFIG_LINK_SAT_SAFE_TIMEOUT_MS
#define LINK_SAT_SAFE_TIMEOUT_MS CONFIG_LINK_SAT_SAFE_TIMEOUT_MS
#else
#define LINK_SAT_SAFE_TIMEOUT_MS 1500
#endif

#if defined(CONFIG_LINK_LBT)
#define LINK_LBT_LISTEN_US      CONFIG_LINK_LBT_LISTEN_US
#define LINK_LBT_MARGIN_DB      CONFIG_LINK_LBT_MARGIN_DB
#define LINK_LBT_BACKOFF_MAX_MS CONFIG_LINK_LBT_BACKOFF_MAX_MS
#define LINK_LBT_MAX_DEFER_MS   CONFIG_LINK_LBT_MAX_DEFER_MS
#elif defined(CONFIG_LINK_TURNAROUND_MS)
/* In an app with listen-before-talk turned off. */
#define LINK_LBT_LISTEN_US      0
#define LINK_LBT_MARGIN_DB      10
#define LINK_LBT_BACKOFF_MAX_MS 10
#define LINK_LBT_MAX_DEFER_MS   0
#else
#define LINK_LBT_LISTEN_US      1000
#define LINK_LBT_MARGIN_DB      10
#define LINK_LBT_BACKOFF_MAX_MS 10
#define LINK_LBT_MAX_DEFER_MS   50
#endif

/*
 * LoRa time on air of a @p len byte packet, rounded up to whole ms: CR 4/5,
 * an 8-symbol preamble, explicit header and CRC, as radio.c sets the modem.
 * The driver's own figure (link_transport.airtime_ms) is the one to use on
 * hardware; this is for the simulator and the tests.
 */
uint32_t link_airtime_ms(size_t len, unsigned int sf, unsigned int bw_khz);

/*
 * The length of one reply slot for replies that take @p reply_airtime_ms,
 * including the listen at its start and the guard time. -EMSGSIZE if it
 * does not fit the poll's one-byte slot length.
 */
int link_slot_ms(uint32_t reply_airtime_ms);

/* When reply slot @p rank starts, counted from the end of the poll. */
static inline int64_t link_slot_start_us(int64_t poll_end_us, uint8_t rank, uint8_t slot_ms)
{
	return poll_end_us + ((int64_t)LINK_TURNAROUND_MS + (int64_t)rank * slot_ms) * 1000;
}

/* When the last of @p count reply slots ends. */
static inline int64_t link_replies_end_us(int64_t poll_end_us, uint8_t count, uint8_t slot_ms)
{
	return link_slot_start_us(poll_end_us, count, slot_ms);
}

/* Seed the backoff generator, once, from a real random source. */
void link_sched_seed(uint32_t seed);

/*
 * Listen before talk. A sender calls link_lbt_begin() and listens at the
 * time it returns, then calls link_lbt_step() with what it heard, and does
 * what that says until it sends or gives up.
 */
struct link_lbt {
	int64_t deadline_us;   /* stop deferring from here on */
	bool force;            /* at the deadline: send anyway, or give up */
};

enum link_lbt_res {
	LINK_LBT_SEND,      /* channel clear */
	LINK_LBT_FORCED,    /* still busy at the deadline, send anyway */
	LINK_LBT_RETRY,     /* busy: listen again at *retry_at_us */
	LINK_LBT_GIVE_UP,   /* still busy at the deadline, drop the frame */
};

/*
 * Start deferring at @p now_us for at most @p max_defer_us. With
 * @p predelay the first listen comes after a random wait of up to one
 * backoff, so that senders woken by the same event do not all find the
 * channel clear at the same instant. Returns the time of the first listen.
 */
int64_t link_lbt_begin(struct link_lbt *l, int64_t now_us, uint32_t max_defer_us, bool force,
		       bool predelay);

/* The listen that ended at @p now_us found the channel @p clear. */
enum link_lbt_res link_lbt_step(struct link_lbt *l, bool clear, int64_t now_us,
				int64_t *retry_at_us);

#endif /* LINK_SCHED_H_ */
