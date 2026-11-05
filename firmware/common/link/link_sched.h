/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * Channel access timing, shared by sat_link, ctrl_link, the controller's
 * satellite simulator and the unit tests: airtime, reply slots, channel
 * utilization and the CSMA/CA policy. Pure: time comes in as an argument, in
 * microseconds. See documentation/radio_link.md, "Channel access".
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
#define LINK_SAT_SAFE_TIMEOUT_MS 4000
#endif

#if defined(CONFIG_LINK_LBT)
#define LINK_LBT_LISTEN_US      CONFIG_LINK_LBT_LISTEN_US
#define LINK_LBT_MARGIN_DB      CONFIG_LINK_LBT_MARGIN_DB
#define LINK_LBT_MAX_DEFER_MS   CONFIG_LINK_LBT_MAX_DEFER_MS
#define LINK_CSMA_SLOT_US       CONFIG_LINK_CSMA_SLOT_US
#define LINK_CSMA_CTRL_CW_MIN   CONFIG_LINK_CSMA_CTRL_CW_MIN
#define LINK_CSMA_CTRL_CW_MAX   CONFIG_LINK_CSMA_CTRL_CW_MAX
#define LINK_CSMA_SAT_CW_MIN    CONFIG_LINK_CSMA_SAT_CW_MIN
#define LINK_CSMA_SAT_CW_MAX    CONFIG_LINK_CSMA_SAT_CW_MAX
#elif defined(CONFIG_LINK_TURNAROUND_MS)
/* In an app with listen-before-talk turned off. */
#define LINK_LBT_LISTEN_US      0
#define LINK_LBT_MARGIN_DB      10
#define LINK_LBT_MAX_DEFER_MS   0
#define LINK_CSMA_SLOT_US       1000
#define LINK_CSMA_CTRL_CW_MIN   0
#define LINK_CSMA_CTRL_CW_MAX   0
#define LINK_CSMA_SAT_CW_MIN    0
#define LINK_CSMA_SAT_CW_MAX    0
#else
#define LINK_LBT_LISTEN_US      1000
#define LINK_LBT_MARGIN_DB      10
#define LINK_LBT_MAX_DEFER_MS   50
#define LINK_CSMA_SLOT_US       1000
#define LINK_CSMA_CTRL_CW_MIN   3
#define LINK_CSMA_CTRL_CW_MAX   7
#define LINK_CSMA_SAT_CW_MIN    4
#define LINK_CSMA_SAT_CW_MAX    8
#endif

#ifdef CONFIG_LINK_UTIL_WINDOW_S
#define LINK_UTIL_WINDOW_S CONFIG_LINK_UTIL_WINDOW_S
#else
#define LINK_UTIL_WINDOW_S 10
#endif

/*
 * LoRa time on air of a @p len byte packet, rounded up to whole ms: CR 4/5,
 * an 8-symbol preamble, explicit header and CRC, as radio.c sets the modem.
 * The driver's own figure (link_transport.airtime_ms) is the one to use on
 * hardware; this is for the simulator and the tests.
 */
uint32_t link_airtime_ms(size_t len, unsigned int sf, unsigned int bw_khz);
/* The same in microseconds, unrounded. */
uint32_t link_airtime_us(size_t len, unsigned int sf, unsigned int bw_khz);

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
 * Channel utilization: the share of the last LINK_UTIL_WINDOW_S seconds in
 * which this device found the channel busy, from any network. Fed with
 * every frame received, every frame sent and every carrier sense that
 * found the channel busy. Overlapping intervals count twice, so the
 * figure is capped at 100 %.
 */
#define LINK_UTIL_BUCKET_US 1000000

struct link_util {
	int64_t since_us;          /* first use */
	int64_t bucket_start_us;   /* start of the current bucket */
	uint8_t cur;
	uint32_t busy_us[LINK_UTIL_WINDOW_S];
};

void link_util_init(struct link_util *u, int64_t now_us);
/* The channel was busy from @p start_us to @p end_us. */
void link_util_add(struct link_util *u, int64_t start_us, int64_t end_us);
/* Busy share of the window up to @p now_us, 0..100. */
unsigned int link_util_pct(struct link_util *u, int64_t now_us);

/*
 * CSMA/CA, the way Meshtastic does it: before a frame the sender waits a
 * random number of slots out of a contention window, then senses the
 * channel. Clear: it sends. Busy: it senses again every slot until the
 * channel clears, then draws a new backoff. The window is 2^cw slots, and
 * cw grows from cw_min to cw_max with the channel utilization, so that a
 * crowded channel spreads the senders out further. A sense takes about
 * LINK_CSMA_SLOT_US.
 *
 * Each kind of frame has its class. The deadline counts from the start,
 * after the full contention window at the chosen cw, plus max_defer_us:
 * there the sender sends anyway (force) or drops the frame.
 */
struct link_csma_class {
	uint8_t cw_min;
	uint8_t cw_max;
	uint32_t max_defer_us;
	bool force;
};

/* Controller: polls and its other frames. A poll carries SAFE and FIRE. */
extern const struct link_csma_class link_csma_ctrl;
/* Satellite: its reply in a poll slot. One sense, then send or skip. */
extern const struct link_csma_class link_csma_slot;
/* Satellite: answers to the controller, into the gap it leaves for them. */
extern const struct link_csma_class link_csma_answer;
/* Satellite: HELLO, PAIR_ADV and unscheduled data, repeated anyway. */
extern const struct link_csma_class link_csma_unsolicited;

struct link_csma {
	const struct link_csma_class *c;
	int64_t deadline_us;
	uint8_t cw;
	bool was_busy;   /* the last sense found the channel busy */
};

enum link_csma_res {
	LINK_CSMA_SEND,      /* channel clear */
	LINK_CSMA_FORCED,    /* still busy at the deadline, send anyway */
	LINK_CSMA_SENSE,     /* sense again at *next_us */
	LINK_CSMA_GIVE_UP,   /* still busy at the deadline, drop the frame */
};

/* The contention window exponent for @p util_pct (0..100). */
uint8_t link_csma_cw(const struct link_csma_class *c, unsigned int util_pct);

/*
 * Start at @p now_us with the channel @p util_pct busy. Returns the time
 * of the first sense, after the random backoff.
 */
int64_t link_csma_begin(struct link_csma *s, const struct link_csma_class *c,
			unsigned int util_pct, int64_t now_us);

/* The sense that ended at @p now_us found the channel @p clear. */
enum link_csma_res link_csma_step(struct link_csma *s, bool clear, int64_t now_us,
				  int64_t *next_us);

/*
 * Other networks on the channel (documentation/radio_link.md, "Optional:
 * other networks on the channel"). A controller hears the frames of other controllers'
 * networks, which its own satellites may not: they are too far away, or
 * below their noise floor. From those frames it estimates when the other
 * networks will be on the air, and places its own frames, and the answers
 * it expects, in the gaps.
 *
 * Per network (its net_tag) it learns the start of the last POLL, the poll
 * period, and how long a cycle takes from its POLL to the end of the last
 * satellite reply heard. A controller frame that expects an answer (a
 * join or a group key update) reserves the answer's window too. A network
 * not heard for LINK_FOREIGN_FORGET_US is forgotten.
 */
#define LINK_FOREIGN_NETS         4
#define LINK_FOREIGN_FORGET_US    5000000
/* Cycle length assumed until one has been observed. */
#define LINK_FOREIGN_CYCLE_US     250000
/* Satellite frames this long after a POLL still belong to its cycle. */
#define LINK_FOREIGN_CYCLE_MAX_US 1000000
/* Slack around a predicted cycle, for the other controller's timing. */
#define LINK_FOREIGN_MARGIN_US    15000

#ifdef CONFIG_LINK_FOREIGN_MAX_WAIT_MS
#define LINK_FOREIGN_MAX_WAIT_MS CONFIG_LINK_FOREIGN_MAX_WAIT_MS
#else
#define LINK_FOREIGN_MAX_WAIT_MS 400
#endif

struct link_foreign_net {
	bool used;
	uint16_t tag;
	int64_t heard_us;          /* end of the last frame heard */
	int64_t poll_us;           /* start of the last POLL, 0 before one */
	int64_t period_us;         /* between POLLs, 0 until measured */
	int64_t cycle_us;          /* POLL start to the end of its replies, 0 unknown */
	int64_t cycle_end_us;      /* end of the current cycle's last reply */
	int64_t reserved_until_us; /* an answer window */
};

struct link_foreign {
	struct link_foreign_net net[LINK_FOREIGN_NETS];
	uint32_t min_cycle_us;     /* see link_foreign_min_cycle() */
};

void link_foreign_init(struct link_foreign *f);

/*
 * Reserve at least @p cycle_us for every other network's cycle: our own
 * cycle's length. A controller often cannot hear another network's
 * satellites (they are far out on the other side), so what it hears of
 * that cycle is the poll alone, and it would put its own poll into the
 * other network's reply slots, right where its satellites and the other
 * network's meet.
 */
void link_foreign_min_cycle(struct link_foreign *f, uint32_t cycle_us);

/* How many other networks have been heard within LINK_FOREIGN_FORGET_US. */
int link_foreign_count(const struct link_foreign *f, int64_t now_us);

/*
 * A frame of network @p tag was heard from @p start_us to @p end_us: a
 * @p poll, or from its controller (@p from_ctrl) or a satellite. A
 * controller frame that expects an answer within @p answer_us of its end
 * reserves that window; 0 if it expects none.
 */
void link_foreign_heard(struct link_foreign *f, uint16_t tag, bool poll, bool from_ctrl,
			int64_t start_us, int64_t end_us, uint32_t answer_us);

/*
 * The earliest time from @p now_us on at which @p need_us of air time
 * (a frame and what it reserves) meets none of the other networks' current
 * or next predicted cycles and answer windows. @p now_us if nothing fits
 * within @p max_wait_us.
 */
int64_t link_foreign_start(const struct link_foreign *f, int64_t now_us, uint32_t need_us,
			   uint32_t max_wait_us);

/*
 * Where our next idle poll should go, DESYNC style (Degesys et al., 2007):
 * every controller moves its own cycle toward the middle of the gap
 * between the other networks' cycles before and after it, so that polls
 * spread evenly over the period instead of meeting the same neighbour
 * every time. Only networks polling within 20 % of our @p period_us count (a
 * network repeating a command polls faster, and link_foreign_start()
 * handles that frame by frame).
 *
 * Our last poll started at @p our_start_us and its cycle, poll and reply
 * slots, took @p our_len_us. Returns how far to move the next poll from
 * our_start_us + period_us: a fraction LINK_DESYNC_ALPHA_PCT of the way
 * to the midpoint, clamped to [-LINK_DESYNC_EARLY_MAX_US,
 * LINK_DESYNC_LATE_MAX_US]; 0 with nobody to space out from, or no room.
 */
#define LINK_DESYNC_ALPHA_PCT      50
#define LINK_DESYNC_EARLY_MAX_US   300000
/* Keeps the gap between two polls under 1.35 s, for the 4 s SAFE timeout. */
#define LINK_DESYNC_LATE_MAX_US    150000

int32_t link_foreign_shift_us(const struct link_foreign *f, int64_t now_us, int64_t our_start_us,
			      uint32_t our_len_us, uint32_t period_us);

#endif /* LINK_SCHED_H_ */
