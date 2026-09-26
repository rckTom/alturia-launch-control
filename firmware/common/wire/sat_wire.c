/* See sat_wire.h. Little-endian throughout. */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "sat_wire.h"

enum wire_type {
	WIRE_STATUS = 2,        /* full status */
	WIRE_POLL = 3,          /* poll, short status wanted */
	WIRE_STATUS_SHORT = 4,
	WIRE_POLL_FULL = 5,     /* poll, full status wanted */
};

size_t wire_encode_poll(const struct link_cmd *cmds, size_t n, uint8_t mask, bool full,
			uint8_t *out)
{
	uint8_t *p = out;

	*p++ = full ? WIRE_POLL_FULL : WIRE_POLL;
	for (size_t sat = 0; sat < n; sat++) {
		if (mask & BIT(sat)) {
			*p++ = cmds[sat].type;
			*p++ = cmds[sat].mask;
			*p++ = cmds[sat].valves;
			*p++ = cmds[sat].id;
		}
	}
	return p - out;
}

int wire_decode_poll(const uint8_t *buf, size_t len, uint8_t rank, uint8_t count,
		     struct link_cmd *cmd, bool *full)
{
	const uint8_t *rec = buf + 1 + rank * WIRE_POLL_CMD_LEN;

	if (len != 1 + (size_t)count * WIRE_POLL_CMD_LEN ||
	    (buf[0] != WIRE_POLL && buf[0] != WIRE_POLL_FULL) || rank >= count ||
	    rec[0] > CMD_FIRE) {
		return -EINVAL;
	}

	cmd->type = rec[0];
	cmd->mask = rec[1];
	cmd->valves = rec[2];
	cmd->id = rec[3];
	if (full) {
		*full = buf[0] == WIRE_POLL_FULL;
	}
	return 0;
}

/*
 * Both sizes start alike: type, seq (u16), state, flags, the masks, fire
 * count, refused channels, request id, continuity (2 bits per channel)
 * and the battery. The full status goes on with the resistances, the rail
 * current and the link quality.
 */
size_t wire_encode_status(const struct sat_status *st, bool full, uint8_t *out)
{
	uint8_t *p = out;
	uint16_t cont = 0;

	for (int ch = 0; ch < NUM_CHANNELS; ch++) {
		cont |= (st->cont[ch] & 0x3) << (2 * ch);
	}

	*p++ = full ? WIRE_STATUS : WIRE_STATUS_SHORT;
	sys_put_le16((uint16_t)st->seq, p);
	p += 2;
	*p++ = st->state;
	*p++ = (st->key_armed ? BIT(0) : 0) | (st->estop ? BIT(1) : 0);
	*p++ = st->armed_mask;
	*p++ = st->firing_mask;
	*p++ = st->valve_mask;
	*p++ = st->fire_count;
	*p++ = st->arm_refused;
	*p++ = st->cmd_id;
	sys_put_le16(cont, p);
	p += 2;
	sys_put_le16(CLAMP(st->vbat_mv, 0, UINT16_MAX), p);
	p += 2;

	if (!full) {
		return p - out;
	}

	for (int ch = 0; ch < NUM_CHANNELS; ch++) {
		sys_put_le32(st->res_mohm[ch], p);
		p += 4;
	}
	sys_put_le16(CLAMP(st->rail_ma, 0, UINT16_MAX), p);
	p += 2;
	sys_put_le16(st->rssi_dbm, p);
	p += 2;
	*p++ = st->snr_db;

	return p - out;
}

int wire_decode_status(const uint8_t *buf, size_t len, struct sat_status *st)
{
	const uint8_t *p = buf;
	bool full;
	uint16_t seq;
	uint16_t cont;

	if (len == WIRE_STATUS_LEN && buf[0] == WIRE_STATUS) {
		full = true;
	} else if (len == WIRE_STATUS_SHORT_LEN && buf[0] == WIRE_STATUS_SHORT) {
		full = false;
	} else {
		return -EINVAL;
	}
	if (buf[3] > SAT_FIRING) {
		return -EINVAL;
	}

	p++;
	seq = sys_get_le16(p);
	p += 2;
	st->seq += (int16_t)(seq - (uint16_t)st->seq);
	st->state = *p++;
	st->key_armed = *p & BIT(0);
	st->estop = *p++ & BIT(1);
	st->armed_mask = *p++;
	st->firing_mask = *p++;
	st->valve_mask = *p++;
	st->fire_count = *p++;
	st->arm_refused = *p++;
	st->cmd_id = *p++;
	cont = sys_get_le16(p);
	p += 2;
	for (int ch = 0; ch < NUM_CHANNELS; ch++) {
		st->cont[ch] = MIN((cont >> (2 * ch)) & 0x3, CONT_SHORT);
	}
	st->vbat_mv = sys_get_le16(p);
	p += 2;

	if (!full) {
		return 0;
	}

	for (int ch = 0; ch < NUM_CHANNELS; ch++) {
		st->res_mohm[ch] = (int32_t)sys_get_le32(p);
		p += 4;
	}
	st->rail_ma = sys_get_le16(p);
	p += 2;
	st->rssi_dbm = (int16_t)sys_get_le16(p);
	p += 2;
	st->snr_db = (int8_t)*p;

	return 0;
}
