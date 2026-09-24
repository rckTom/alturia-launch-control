/*
 * Controller <-> satellite link interface.
 *
 * Implemented by link_lsec.c on the secure link in ../common/link: every
 * command and status travels sealed (AES-CCM, replay-checked) between
 * paired devices, see design/radio_security.md. The frames go over the
 * LoRa radio, or with CONFIG_APP_SAT_SIM through sat_sim.c's simulated
 * air to simulated satellites that run the same crypto.
 * All quantities are integers (mV, mA, mOhm) so no float formatting is needed.
 */

#ifndef LINK_H_
#define LINK_H_

#include <stdbool.h>
#include <stdint.h>

#include "lsec.h"

#define NUM_SATS     2
#define NUM_CHANNELS 6

/* Reported resistance for "no measurable connection". */
#define RES_OPEN_MOHM INT32_MAX

enum sat_state {
	SAT_SAFE,
	SAT_ARMED,
	SAT_FIRING,
};

enum continuity {
	CONT_OK,
	CONT_OPEN,
	CONT_SHORT,
};

struct sat_status {
	uint32_t seq;
	enum sat_state state;
	bool key_armed;       /* pad key switch in ARM position */
	bool estop;           /* pad E-STOP pressed */
	uint8_t armed_mask;   /* channels the satellite accepted for arming */
	uint8_t firing_mask;  /* channels currently driven by a fire pulse */
	uint8_t valve_mask;   /* channels continuously energized (valves) */
	uint8_t fire_count;   /* incremented for every accepted FIRE command */
	uint8_t arm_refused;  /* channels that failed the pre-arm short check */
	int32_t res_mohm[NUM_CHANNELS];
	enum continuity cont[NUM_CHANNELS];
	int32_t vbat_mv;
	int32_t rail_ma;
	int16_t rssi_dbm;
	int8_t snr_db;
};

enum link_cmd_type {
	CMD_HEARTBEAT,
	CMD_ARM,
	CMD_DISARM,
	CMD_FIRE,
};

/*
 * Every command carries the valve outputs the controller wants energized.
 * The satellite applies them only while armed, and only to armed channels,
 * so a lost packet is corrected by the next heartbeat.
 */
struct link_cmd {
	enum link_cmd_type type;
	uint8_t mask;    /* ARM: channels to arm. FIRE: channels to pulse. */
	uint8_t valves;
};

/*
 * Poll every joined satellite: one broadcast carrying cmds[sat] for each,
 * which each answers with its status in its own reply slot. The poll may
 * be lost. -ENOTCONN if no satellite is joined, -EBUSY if polls are
 * queueing up faster than they can be sent.
 */
int link_poll(const struct link_cmd cmds[NUM_SATS]);

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
