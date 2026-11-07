/*
 * Command and status payloads between the controller and its satellites,
 * as they travel inside sealed lsec frames: the commands inside the
 * controller's poll, and each satellite's status in its reply slot.
 * Shared by the controller (link_lsec.c, sat_sim.c) and the satellite.
 *
 * Commands are requests with an id. A satellite executes a request once:
 * a command whose id it executed last is not executed again, and its
 * status reports that id, which is how the controller knows the request
 * arrived. So the controller can repeat a command, FIRE included, until
 * it is acknowledged. Id 0 is no request (a heartbeat). See
 * documentation/radio_link.md, "Commands and status".
 *
 * All quantities are integers (mV, mA, mOhm) so no float formatting is needed.
 */

#ifndef SAT_WIRE_H_
#define SAT_WIRE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ignlog_rec.h"

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
	uint8_t arm_refused;  /* channels that failed ARM's checks: open, FET, high side */
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
 * networks share the channel (documentation/radio_link.md, "Commands and status").
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

/*
 * Ignition log download (documentation/ignition_log.md). A log poll
 * addresses one satellite, carries no command, and asks for either the
 * index of its records or one chunk of a record. The satellite answers in
 * its slot with the index or the chunk instead of its status. The
 * controller sends log polls only while nothing on the network is armed.
 *
 *   log poll  [6][op u8][seq u32][off u16]
 *   index     [7][store_id u16][boots u16][now_ms u32][scale u32][count u8]
 *             count x [seq u32][t_fire_ms u32][boot u16][n u16][period_us u16]
 *                     [peak u16][open_idx u16][channel u8][flags u8]
 *   chunk     [8][seq u32][off u16][total u16][data]
 *
 * A chunk request for a record the satellite no longer has is answered
 * with the index, so the controller learns what is there.
 */
enum wire_log_op {
	WIRE_LOG_OP_INDEX,
	WIRE_LOG_OP_CHUNK,
};

#define WIRE_LOG_POLL_LEN      8
#define WIRE_LOG_INDEX_MAX     8
#define WIRE_LOG_ENTRY_LEN     20
#define WIRE_LOG_INDEX_LEN(n)  (14 + (n) * WIRE_LOG_ENTRY_LEN)
#define WIRE_LOG_CHUNK_HDR     9
#define WIRE_LOG_CHUNK_MAX     192
#define WIRE_LOG_CHUNK_LEN     (WIRE_LOG_CHUNK_HDR + WIRE_LOG_CHUNK_MAX)

struct wire_log_req {
	uint8_t op;      /* enum wire_log_op */
	uint32_t seq;    /* WIRE_LOG_OP_CHUNK: the record */
	uint16_t off;    /* and the offset into it */
};

struct wire_log_index {
	uint16_t store_id;  /* changes when the satellite's store starts over */
	uint16_t boots;     /* the satellite's boot count now */
	uint32_t now_ms;    /* and its uptime, for the age of this boot's records */
	uint32_t scale_na;  /* nA per code, as in the records */
	uint8_t count;
	struct ignlog_entry e[WIRE_LOG_INDEX_MAX];
};

struct wire_log_chunk {
	uint32_t seq;
	uint16_t off;
	uint16_t total;      /* the record's size */
	uint8_t len;
	const uint8_t *data; /* into the decoded buffer */
};

enum wire_reply_kind {
	WIRE_REPLY_STATUS,
	WIRE_REPLY_LOG_INDEX,
	WIRE_REPLY_LOG_CHUNK,
	WIRE_REPLY_UNKNOWN,
};

/* What a satellite's reply carries, by its type byte. */
enum wire_reply_kind wire_reply_kind(const uint8_t *buf, size_t len);

size_t wire_encode_log_poll(const struct wire_log_req *req, uint8_t *out);
/* -EINVAL unless @p buf is a log poll. */
int wire_decode_log_poll(const uint8_t *buf, size_t len, struct wire_log_req *req);

/* @p out holds WIRE_LOG_INDEX_LEN(WIRE_LOG_INDEX_MAX). */
size_t wire_encode_log_index(const struct wire_log_index *ix, uint8_t *out);
int wire_decode_log_index(const uint8_t *buf, size_t len, struct wire_log_index *ix);

/* @p out holds WIRE_LOG_CHUNK_LEN; c->len is at most WIRE_LOG_CHUNK_MAX. */
size_t wire_encode_log_chunk(const struct wire_log_chunk *c, uint8_t *out);
int wire_decode_log_chunk(const uint8_t *buf, size_t len, struct wire_log_chunk *c);

#endif /* SAT_WIRE_H_ */
