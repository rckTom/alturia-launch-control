/*
 * Controller <-> satellite link interface.
 *
 * On native_sim this is implemented by sat_sim.c. On hardware it would be
 * the LoRa transport with HMAC and nonces; the controller does not care.
 * All quantities are integers (mV, mA, mOhm) so no float formatting is needed.
 */

#ifndef LINK_H_
#define LINK_H_

#include <stdbool.h>
#include <stdint.h>

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

/* Queue a command for transmission to one satellite. It may be lost. */
void link_send(int sat, const struct link_cmd *cmd);

/*
 * Copy the most recently received status of one satellite. Returns the
 * uptime in ms at which it was received, or -1 if none was received yet.
 */
int64_t link_last_status(int sat, struct sat_status *out);

#endif /* LINK_H_ */
