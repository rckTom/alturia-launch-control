/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The satellite's end of the secure LoRa link: lsec on top of a transport
 * (normally the shared radio backend), with its own receive thread, pairing storage and the
 * switch to the pairing radio profile. The satellite firmware and the
 * bench's satellite role both run exactly this.
 *
 * The controller polls; the satellite speaks when asked. On LSEC_EV_POLL
 * the handler calls sat_link_reply(), and the link sends that reply in the
 * satellite's slot of the poll cycle, after listening that the channel is
 * clear. Everything else it sends (HELLO, join and pairing answers) waits
 * for a clear channel first. See design/radio_security.md, "Channel access".
 *
 * Every function may be called from any thread. Events arrive on the link
 * thread; a handler may call back into sat_link_*.
 */

#ifndef SAT_LINK_H_
#define SAT_LINK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link_access.h"
#include "link_transport.h"
#include "lsec.h"

typedef void (*link_event_cb)(const struct lsec_event *ev);

struct sat_link_state {
	uint64_t uid;
	uint8_t slot;
	bool paired;
	bool joined;
	bool pairing;
	int16_t rssi_dbm;   /* of the last received packet */
	int8_t snr_db;
	struct lsec_stats stats;

	uint32_t polls;          /* addressed to this satellite */
	uint32_t replies;        /* sent in their slot */
	uint32_t slot_busy;      /* slots skipped: someone else was on the air */
	uint32_t slot_late;      /* slots skipped: missed their start */
	struct link_lbt_stats lbt;   /* frames sent outside a slot */
	int64_t last_poll_us;    /* reception of the last addressed poll, link_now_us() */
	int64_t last_reply_us;   /* start of the last reply */
};

/*
 * Load the stored pairing and start the link thread. Frames go through
 * @p transport, usually &link_radio_transport after radio_init(). Needs
 * settings_subsys_init(). The uid is derived from hwinfo.
 */
int sat_link_init(link_event_cb cb, const struct link_transport *transport);

/*
 * Answer the poll being handled; call it from the LSEC_EV_POLL handler.
 * The reply goes out in this satellite's slot. Without a call an empty
 * frame goes out instead, so the controller still hears from it.
 * -EAGAIN outside a poll, -EMSGSIZE if the reply would not fit the slot.
 */
int sat_link_reply(const void *data, size_t len);

/*
 * Seal and send to the controller outside the poll cycle, after listening
 * for a clear channel. -ENOTCONN without a session, -EBUSY while a reply
 * waits for its slot. Prefer sat_link_reply(): an unscheduled frame can
 * collide with the other satellites' replies.
 */
int sat_link_send(const void *data, size_t len);

/*
 * Enter pairing mode for LSEC_PAIR_WINDOW_MS. The caller has checked that
 * the satellite is SAFE and that the user made the physical gesture.
 */
int sat_link_pair_start(void);
void sat_link_pair_confirm(bool accept);
int sat_link_unpair(void);

void sat_link_state(struct sat_link_state *out);

/*
 * Bench only, built with LINK_TEST_HOOKS: send the last sealed DATA frame
 * again, byte for byte, as an attacker replaying it would.
 */
int sat_link_replay_last(void);

#endif /* SAT_LINK_H_ */
