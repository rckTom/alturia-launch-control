/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * The controller's end of the secure LoRa link: lsec on top of a transport
 * (normally the shared radio backend), with its own receive thread, the network id and pairing
 * storage, and the switch to the pairing radio profile while scanning or
 * pairing. The controller firmware and the bench's controller role both
 * run exactly this.
 *
 * The link thread is the only one that transmits. Frames go out in the
 * order they were sealed, each after CSMA/CA: a random backoff that grows
 * with the channel utilization, then a carrier sense (and sent anyway
 * after LINK_LBT_MAX_DEFER_MS beyond the backoff). After a poll the controller
 * stays quiet until every reply slot has passed; after any other frame it
 * leaves a gap for the satellite's answer. See documentation/radio_link.md,
 * "Channel access".
 *
 * Every function may be called from any thread. Events arrive on the link
 * thread; a handler may call back into ctrl_link_*.
 */

#ifndef CTRL_LINK_H_
#define CTRL_LINK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link_access.h"
#include "link_transport.h"
#include "lsec.h"
#include "sat_link.h"

struct ctrl_link_state {
	uint32_t net_id;
	bool scanning;
	bool pairing;
	int16_t rssi_dbm;   /* of the last received packet */
	int8_t snr_db;
	struct lsec_stats stats;

	uint8_t joined_mask;      /* bit n: slot n+1 has a session */
	uint32_t polls;           /* sent */
	uint32_t held;            /* frames held for a gap in another network's air time */
	uint32_t poll_q_full;     /* poll requests refused */
	uint32_t tx_q_full;       /* frames dropped before sending */
	struct link_csma_stats csma;
	uint8_t util_pct;         /* channel busy, every network, last LINK_UTIL_WINDOW_S */
	uint8_t foreign_nets;     /* other networks heard lately */
	int64_t last_poll_end_us; /* when the last poll had been sent, link_now_us() */
	int64_t last_rx_us;       /* reception of the last frame */
};

/*
 * Load the network id (made on first boot) and the pairings, and start the
 * link thread. Frames go through @p transport, usually
 * &link_radio_transport after radio_init(). Needs settings_subsys_init().
 */
int ctrl_link_init(link_event_cb cb, const struct link_transport *transport);

/* Seal and send to a slot or LSEC_ADDR_GROUP. -ENOTCONN without a session. */
int ctrl_link_send(uint8_t dst, const void *data, size_t len);

/*
 * Queue a poll: bit n of @p mask asks slot n+1 to reply, in the order of
 * the set bits, with at most @p reply_len bytes. @p data carries whatever
 * the app has for them, in the same order. Polls go out in the order they
 * were queued; none is ever replaced by a later one. -EBUSY when the queue
 * is full, -EAGAIN while scanning or pairing, -EMSGSIZE if the data or the
 * reply slots do not fit.
 */
int ctrl_link_poll(uint8_t mask, const void *data, size_t len, size_t reply_len);

/*
 * When the next regular poll, @p period_ms after the last, should start,
 * in k_uptime_get() ms: one period after the last poll actually went out
 * (a poll held for another network keeps its new phase), moved toward the
 * middle of the gap between the other networks' cycles
 * (link_foreign_shift_us()). 0 while a queued poll has not gone out yet.
 */
int64_t ctrl_link_next_poll_ms(uint32_t period_ms);

/* Report satellites in pairing mode as LSEC_EV_ADV while on. */
void ctrl_link_scan(bool on);
/* The caller has checked that nothing is armed. */
int ctrl_link_pair_start(uint64_t uid, uint8_t slot);
void ctrl_link_pair_confirm(bool accept);
int ctrl_link_forget(uint8_t slot);

bool ctrl_link_joined(uint8_t slot);
/* Bit n set when slot n+1 has a session. */
uint8_t ctrl_link_joined_mask(void);
/* The paired satellite's uid, or 0 for an empty slot. */
uint64_t ctrl_link_paired_uid(uint8_t slot);
void ctrl_link_state(struct ctrl_link_state *out);

/*
 * Bench only, built with LINK_TEST_HOOKS: send the last sealed DATA frame
 * again, byte for byte, as an attacker replaying it would.
 */
int ctrl_link_replay_last(void);

#endif /* CTRL_LINK_H_ */
