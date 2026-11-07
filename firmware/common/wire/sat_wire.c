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
	WIRE_POLL_LOG = 6,      /* log poll, one satellite */
	WIRE_LOG_INDEX = 7,
	WIRE_LOG_CHUNK = 8,
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
	/* Bit 1 was an E-STOP that the satellite does not have; keep it clear */
	*p++ = st->key_armed ? BIT(0) : 0;
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
	st->key_armed = *p++ & BIT(0);
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

enum wire_reply_kind wire_reply_kind(const uint8_t *buf, size_t len)
{
	if (len == 0) {
		return WIRE_REPLY_UNKNOWN;
	}
	switch (buf[0]) {
	case WIRE_STATUS:
	case WIRE_STATUS_SHORT:
		return WIRE_REPLY_STATUS;
	case WIRE_LOG_INDEX:
		return WIRE_REPLY_LOG_INDEX;
	case WIRE_LOG_CHUNK:
		return WIRE_REPLY_LOG_CHUNK;
	default:
		return WIRE_REPLY_UNKNOWN;
	}
}

size_t wire_encode_log_poll(const struct wire_log_req *req, uint8_t *out)
{
	out[0] = WIRE_POLL_LOG;
	out[1] = req->op;
	sys_put_le32(req->seq, out + 2);
	sys_put_le16(req->off, out + 6);
	return WIRE_LOG_POLL_LEN;
}

int wire_decode_log_poll(const uint8_t *buf, size_t len, struct wire_log_req *req)
{
	if (len != WIRE_LOG_POLL_LEN || buf[0] != WIRE_POLL_LOG || buf[1] > WIRE_LOG_OP_CHUNK) {
		return -EINVAL;
	}
	req->op = buf[1];
	req->seq = sys_get_le32(buf + 2);
	req->off = sys_get_le16(buf + 6);
	return 0;
}

size_t wire_encode_log_index(const struct wire_log_index *ix, uint8_t *out)
{
	uint8_t count = MIN(ix->count, WIRE_LOG_INDEX_MAX);
	uint8_t *p = out;

	*p++ = WIRE_LOG_INDEX;
	sys_put_le16(ix->store_id, p);
	p += 2;
	sys_put_le16(ix->boots, p);
	p += 2;
	sys_put_le32(ix->now_ms, p);
	p += 4;
	sys_put_le32(ix->scale_na, p);
	p += 4;
	*p++ = count;
	for (uint8_t i = 0; i < count; i++) {
		const struct ignlog_entry *e = &ix->e[i];

		sys_put_le32(e->seq, p);
		sys_put_le32(e->t_fire_ms, p + 4);
		sys_put_le16(e->boot, p + 8);
		sys_put_le16(e->n_samples, p + 10);
		sys_put_le16(e->period_us, p + 12);
		sys_put_le16(e->peak_code, p + 14);
		sys_put_le16(e->open_idx, p + 16);
		p[18] = e->channel;
		p[19] = e->flags;
		p += WIRE_LOG_ENTRY_LEN;
	}
	return p - out;
}

int wire_decode_log_index(const uint8_t *buf, size_t len, struct wire_log_index *ix)
{
	const uint8_t *p;

	if (len < WIRE_LOG_INDEX_LEN(0) || buf[0] != WIRE_LOG_INDEX ||
	    buf[13] > WIRE_LOG_INDEX_MAX || len != WIRE_LOG_INDEX_LEN(buf[13])) {
		return -EINVAL;
	}

	ix->store_id = sys_get_le16(buf + 1);
	ix->boots = sys_get_le16(buf + 3);
	ix->now_ms = sys_get_le32(buf + 5);
	ix->scale_na = sys_get_le32(buf + 9);
	ix->count = buf[13];
	p = buf + WIRE_LOG_INDEX_LEN(0);
	for (uint8_t i = 0; i < ix->count; i++) {
		struct ignlog_entry *e = &ix->e[i];

		e->seq = sys_get_le32(p);
		e->t_fire_ms = sys_get_le32(p + 4);
		e->boot = sys_get_le16(p + 8);
		e->n_samples = sys_get_le16(p + 10);
		e->period_us = sys_get_le16(p + 12);
		e->peak_code = sys_get_le16(p + 14);
		e->open_idx = sys_get_le16(p + 16);
		e->channel = p[18];
		e->flags = p[19];
		p += WIRE_LOG_ENTRY_LEN;
	}
	return 0;
}

size_t wire_encode_log_chunk(const struct wire_log_chunk *c, uint8_t *out)
{
	uint8_t len = MIN(c->len, WIRE_LOG_CHUNK_MAX);

	out[0] = WIRE_LOG_CHUNK;
	sys_put_le32(c->seq, out + 1);
	sys_put_le16(c->off, out + 5);
	sys_put_le16(c->total, out + 7);
	memcpy(out + WIRE_LOG_CHUNK_HDR, c->data, len);
	return WIRE_LOG_CHUNK_HDR + len;
}

int wire_decode_log_chunk(const uint8_t *buf, size_t len, struct wire_log_chunk *c)
{
	if (len < WIRE_LOG_CHUNK_HDR || len > WIRE_LOG_CHUNK_LEN || buf[0] != WIRE_LOG_CHUNK) {
		return -EINVAL;
	}
	c->seq = sys_get_le32(buf + 1);
	c->off = sys_get_le16(buf + 5);
	c->total = sys_get_le16(buf + 7);
	c->len = len - WIRE_LOG_CHUNK_HDR;
	c->data = buf + WIRE_LOG_CHUNK_HDR;
	if ((uint32_t)c->off + c->len > c->total) {
		return -EINVAL;
	}
	return 0;
}
