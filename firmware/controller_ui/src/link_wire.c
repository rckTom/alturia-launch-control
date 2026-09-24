/* See link_wire.h. Little-endian throughout. */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "link_wire.h"

enum wire_type {
	WIRE_STATUS = 2,
	WIRE_POLL = 3,
};

size_t wire_encode_poll(const struct link_cmd cmds[NUM_SATS], uint8_t mask, uint8_t *out)
{
	uint8_t *p = out;

	*p++ = WIRE_POLL;
	for (int sat = 0; sat < NUM_SATS; sat++) {
		if (mask & BIT(sat)) {
			*p++ = cmds[sat].type;
			*p++ = cmds[sat].mask;
			*p++ = cmds[sat].valves;
		}
	}
	return p - out;
}

int wire_decode_poll(const uint8_t *buf, size_t len, uint8_t rank, uint8_t count,
		     struct link_cmd *cmd)
{
	const uint8_t *rec = buf + 1 + rank * WIRE_POLL_CMD_LEN;

	if (len != 1 + (size_t)count * WIRE_POLL_CMD_LEN || buf[0] != WIRE_POLL ||
	    rank >= count || rec[0] > CMD_FIRE) {
		return -EINVAL;
	}

	cmd->type = rec[0];
	cmd->mask = rec[1];
	cmd->valves = rec[2];
	return 0;
}

size_t wire_encode_status(const struct sat_status *st, uint8_t *out)
{
	uint8_t *p = out;
	uint16_t cont = 0;

	*p++ = WIRE_STATUS;
	sys_put_le32(st->seq, p);
	p += 4;
	*p++ = st->state;
	*p++ = (st->key_armed ? BIT(0) : 0) | (st->estop ? BIT(1) : 0);
	*p++ = st->armed_mask;
	*p++ = st->firing_mask;
	*p++ = st->valve_mask;
	*p++ = st->fire_count;
	*p++ = st->arm_refused;
	for (int ch = 0; ch < NUM_CHANNELS; ch++) {
		sys_put_le32(st->res_mohm[ch], p);
		p += 4;
		cont |= (st->cont[ch] & 0x3) << (2 * ch);
	}
	sys_put_le16(cont, p);
	p += 2;
	sys_put_le16(CLAMP(st->vbat_mv, 0, UINT16_MAX), p);
	p += 2;
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
	uint16_t cont;

	if (len != WIRE_STATUS_LEN || buf[0] != WIRE_STATUS || buf[5] > SAT_FIRING) {
		return -EINVAL;
	}

	p++;
	st->seq = sys_get_le32(p);
	p += 4;
	st->state = *p++;
	st->key_armed = *p & BIT(0);
	st->estop = *p++ & BIT(1);
	st->armed_mask = *p++;
	st->firing_mask = *p++;
	st->valve_mask = *p++;
	st->fire_count = *p++;
	st->arm_refused = *p++;
	for (int ch = 0; ch < NUM_CHANNELS; ch++) {
		st->res_mohm[ch] = (int32_t)sys_get_le32(p);
		p += 4;
	}
	cont = sys_get_le16(p);
	p += 2;
	for (int ch = 0; ch < NUM_CHANNELS; ch++) {
		st->cont[ch] = MIN((cont >> (2 * ch)) & 0x3, CONT_SHORT);
	}
	st->vbat_mv = sys_get_le16(p);
	p += 2;
	st->rail_ma = sys_get_le16(p);
	p += 2;
	st->rssi_dbm = (int16_t)sys_get_le16(p);
	p += 2;
	st->snr_db = (int8_t)*p;

	return 0;
}
