/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The controller's end of the secure LoRa link: lsec on top of a transport
 * (normally the shared radio backend), with its own receive thread, the network id and pairing
 * storage, and the switch to the pairing radio profile while scanning or
 * pairing. The controller firmware and the bench's controller role both
 * run exactly this.
 *
 * Every function may be called from any thread. Events arrive on the link
 * thread; a handler may call back into ctrl_link_*.
 */

#ifndef CTRL_LINK_H_
#define CTRL_LINK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
};

/*
 * Load the network id (made on first boot) and the pairings, and start the
 * link thread. Frames go through @p transport, usually
 * &link_radio_transport after radio_init(). Needs settings_subsys_init().
 */
int ctrl_link_init(link_event_cb cb, const struct link_transport *transport);

/* Seal and send to a slot or LSEC_ADDR_GROUP. -ENOTCONN without a session. */
int ctrl_link_send(uint8_t dst, const void *data, size_t len);

/* Report satellites in pairing mode as LSEC_EV_ADV while on. */
void ctrl_link_scan(bool on);
/* The caller has checked that nothing is armed. */
int ctrl_link_pair_start(uint64_t uid, uint8_t slot);
void ctrl_link_pair_confirm(bool accept);
int ctrl_link_forget(uint8_t slot);

bool ctrl_link_joined(uint8_t slot);
/* The paired satellite's uid, or 0 for an empty slot. */
uint64_t ctrl_link_paired_uid(uint8_t slot);
void ctrl_link_state(struct ctrl_link_state *out);

/*
 * Bench only, built with LINK_TEST_HOOKS: send the last sealed DATA frame
 * again, byte for byte, as an attacker replaying it would.
 */
int ctrl_link_replay_last(void);

#endif /* CTRL_LINK_H_ */
