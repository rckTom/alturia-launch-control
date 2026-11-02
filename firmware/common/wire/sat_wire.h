/*
 * Command and status payloads between the controller and its satellites,
 * as they travel inside sealed lsec frames: the commands inside the
 * controller's poll, and each satellite's status in its reply slot.
 * Shared by controller_ui (link_lsec.c, sat_sim.c) and the satellite.
 *
 * Commands are requests with an id. A satellite executes a request once:
 * a command whose id it executed last is not executed again, and its
 * status reports that id, which is how the controller knows the request
 * arrived. So the controller can repeat a command, FIRE included, until
 * it is acknowledged. Id 0 is no request (a heartbeat). See
 * design/radio_security.md, "Commands".
 *
 * All quantities are integers (mV, mA, mOhm) so no float formatting is needed.
 */

#ifndef SAT_WIRE_H_
#define SAT_WIRE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
	uint8_t armed_mask;   /* channels the satellite accepted for arming */
	uint8_t firing_mask;  /* channels currently driven by a fire pulse */
	uint8_t valve_mask;   /* channels continuously energized (valves) */
	uint8_t fire_count;   /* incremented for every accepted FIRE command */
	uint8_t arm_refused;  /* channels that failed the pre-arm short check */
	uint8_t cmd_id;       /* the last request executed, 0 for none yet */
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
 * on every poll, so a lost packet is corrected by the next one.
 */
struct link_cmd {
	enum link_cmd_type type;
	uint8_t mask;    /* ARM: channels to arm. FIRE: channels to pulse. */
	uint8_t valves;
	uint8_t id;      /* request id; 0 with CMD_HEARTBEAT */
};

/*
 * A poll: a type byte, then one command record per addressed satellite.
 * The type asks for the short status (the usual) or the full one.
 */
#define WIRE_POLL_CMD_LEN      4
#define WIRE_POLL_MAX_LEN(n)   (1 + (n) * WIRE_POLL_CMD_LEN)

/*
 * The status comes in two sizes. The short one, in reply to every poll,
 * carries what the controller acts on: state, masks, fire count, the last
 * request id, continuity and the battery. The full one adds the measured
 * resistances, the rail current and the link quality; the controller asks
 * for it only now and then, since every reply slot of a poll is as long
 * as the longest reply it allows. Short replies are what lets several
 * networks share the channel (design/radio_security.md, "Commands").
 */
#define WIRE_STATUS_SHORT_LEN  15
#define WIRE_STATUS_LEN        44

/*
 * The commands for the satellites in @p mask (bit n = cmds[n], of @p n),
 * in the order of the set bits, which is the order of their reply slots.
 * @p full asks them for the full status.
 */
size_t wire_encode_poll(const struct link_cmd *cmds, size_t n, uint8_t mask, bool full,
			uint8_t *out);
/*
 * Take the command for the satellite at @p rank of @p count addressed, and
 * whether the poll asks for the full status (@p full may be NULL).
 */
int wire_decode_poll(const uint8_t *buf, size_t len, uint8_t rank, uint8_t count,
		     struct link_cmd *cmd, bool *full);

/* The short status, or the full one; @p out holds WIRE_STATUS_LEN. */
size_t wire_encode_status(const struct sat_status *st, bool full, uint8_t *out);
/*
 * Merge a received status, short or full, into @p st, which holds the
 * previous one (or zeroes): a short status leaves the full-only fields as
 * they were. seq travels as 16 bits and is extended against st->seq, so
 * differences between seqs stay meaningful.
 */
int wire_decode_status(const uint8_t *buf, size_t len, struct sat_status *st);

#endif /* SAT_WIRE_H_ */
