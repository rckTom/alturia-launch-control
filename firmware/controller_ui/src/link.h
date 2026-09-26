/*
 * Controller <-> satellite link interface.
 *
 * Implemented by link_lsec.c on the secure link in ../common/link: every
 * command and status travels sealed (AES-CCM, replay-checked) between
 * paired devices, see design/radio_security.md. The frames go over the
 * LoRa radio, or with CONFIG_APP_SAT_SIM through sat_sim.c's simulated
 * air to simulated satellites that run the same crypto.
 */

#ifndef LINK_H_
#define LINK_H_

#include <stdbool.h>
#include <stdint.h>

#include "lsec.h"
#include "sat_wire.h"

#define NUM_SATS 2

/*
 * Poll every joined satellite: one broadcast carrying cmds[sat] for each,
 * which each answers with its status in its own reply slot, the short one
 * unless @p full. The poll may be lost. -ENOTCONN if no satellite is
 * joined, -EBUSY if polls are queueing up faster than they can be sent.
 */
int link_poll(const struct link_cmd cmds[NUM_SATS], bool full);

/*
 * Copy the most recently received status of one satellite. Returns the
 * uptime in ms at which it was received, or -1 if none was received yet.
 */
int64_t link_last_status(int sat, struct sat_status *out);

/* Start the secure link. Needs settings_subsys_init() first. */
int link_init(void);

/*
 * Pairing. Satellite sat uses lsec slot sat + 1. Outcomes arrive as
 * events, see link_pair_event().
 */
void link_scan(bool on);
int link_pair(int sat, uint64_t uid);
void link_pair_confirm(bool accept);
int link_forget(int sat);
/* The paired satellite's uid, 0 if the slot is empty. */
uint64_t link_paired_uid(int sat);
bool link_joined(int sat);

/*
 * When the next regular poll should go out, in uptime ms: a period after
 * the last one actually did, placed in the gap between other networks'
 * polls. 0 while the last poll is still waiting to go out.
 */
int64_t link_next_poll_ms(uint32_t period_ms);

/*
 * How busy the channel is, every network counted, in % of the last few
 * seconds, and how many other networks the controller hears.
 */
void link_channel(uint8_t *util_pct, uint8_t *other_nets);

/* A pairing event for the controller: ADV, SAS, PAIRED or PAIR_FAILED. */
struct link_pair_event {
	enum lsec_event_type type;
	int sat;
	uint64_t uid;
	int16_t rssi_dbm;
	enum lsec_fail reason;
	uint8_t sas[LSEC_SAS_FRAMES];
};

/* Take the next pairing event; false if there is none. */
bool link_pair_event(struct link_pair_event *ev);

#endif /* LINK_H_ */
