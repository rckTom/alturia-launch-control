/* SPDX-License-Identifier: Apache-2.0 */

/*
 * One line per frame, for example:
 *
 *   rx HELLO        1 -> 0   net 3a41, 5 B, -45 dBm 9 dB
 *   tx JOIN_REQ     0 -> 1   net 3a41, 13 B
 *   tx DATA         0 -> ff  net 3a41 kid 2 ctr 17, 49 B
 *   rx PAIR_ADV     uid 00001c6fa68fcba4, 9 B, -40 dBm 10 dB
 *
 * Addresses: 0 is the controller, ff the group, anything else a
 * satellite's slot.
 */

#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "lsec_internal.h"
#include "trace.h"

LOG_MODULE_REGISTER(trace, LOG_LEVEL_INF);

static atomic_t mode = ATOMIC_INIT(TRACE_OFF);

static const char *kind_name(int kind)
{
	switch (kind) {
	case KIND_PAIR_ADV:
		return "PAIR_ADV";
	case KIND_PAIR_REQ:
		return "PAIR_REQ";
	case KIND_PAIR_RSP:
		return "PAIR_RSP";
	case KIND_PAIR_NC:
		return "PAIR_NC";
	case KIND_PAIR_NS:
		return "PAIR_NS";
	case KIND_PAIR_CHK_C:
		return "PAIR_CHK_C";
	case KIND_PAIR_CHK_S:
		return "PAIR_CHK_S";
	case KIND_PAIR_ABORT:
		return "PAIR_ABORT";
	case KIND_HELLO:
		return "HELLO";
	case KIND_JOIN_REQ:
		return "JOIN_REQ";
	case KIND_JOIN_RSP:
		return "JOIN_RSP";
	case KIND_JOIN_OK:
		return "JOIN_OK";
	case KIND_JOIN_ACK:
		return "JOIN_ACK";
	case KIND_GROUP_UPDATE:
		return "GROUP_UPDATE";
	case KIND_GROUP_ACK:
		return "GROUP_ACK";
	case KIND_DATA:
		return "DATA";
	default:
		return NULL;
	}
}

/* @p rssi_dbm is NULL for a transmitted frame. */
static void log_frame(const char *dir, const uint8_t *buf, size_t len, const int16_t *rssi_dbm,
		      const int8_t *snr_db)
{
	int kind = lsec_frame_kind(buf, len);
	const char *name = kind_name(kind);
	int16_t rssi = rssi_dbm ? *rssi_dbm : 0;
	int8_t snr = snr_db ? *snr_db : 0;
	struct lsec_hdr h;

	if (!name) {
		/* Wrong version, or not an lsec frame at all. */
		if (rssi_dbm) {
			LOG_INF("%s ?%02x, %zu B, %d dBm %d dB", dir, len ? buf[0] : 0, len,
				rssi, snr);
		} else {
			LOG_INF("%s ?%02x, %zu B", dir, len ? buf[0] : 0, len);
		}
	} else if (KIND_IS_PAIR(kind)) {
		uint64_t uid = len >= PAIR_HDR ? get_le64(&buf[1]) : 0;

		if (rssi_dbm) {
			LOG_INF("%s %-12s uid %016llx, %zu B, %d dBm %d dB", dir, name, uid, len,
				rssi, snr);
		} else {
			LOG_INF("%s %-12s uid %016llx, %zu B", dir, name, uid, len);
		}
	} else if (lsec_parse_hdr(buf, len, &h) != 0) {
		LOG_INF("%s %-12s truncated, %zu B", dir, name, len);
	} else if (KIND_IS_SEALED(kind)) {
		if (rssi_dbm) {
			LOG_INF("%s %-12s %x -> %-2x  net %04x kid %u ctr %u, %zu B, %d dBm %d dB",
				dir, name, h.src, h.dst, h.net_tag, h.kid, h.ctr, len, rssi, snr);
		} else {
			LOG_INF("%s %-12s %x -> %-2x  net %04x kid %u ctr %u, %zu B", dir, name,
				h.src, h.dst, h.net_tag, h.kid, h.ctr, len);
		}
	} else {
		if (rssi_dbm) {
			LOG_INF("%s %-12s %x -> %-2x  net %04x, %zu B, %d dBm %d dB", dir, name,
				h.src, h.dst, h.net_tag, len, rssi, snr);
		} else {
			LOG_INF("%s %-12s %x -> %-2x  net %04x, %zu B", dir, name, h.src, h.dst,
				h.net_tag, len);
		}
	}

	if (atomic_get(&mode) == TRACE_HEX) {
		LOG_HEXDUMP_INF(buf, len, "");
	}
}

static int send(const void *buf, size_t len)
{
	if (atomic_get(&mode) != TRACE_OFF) {
		log_frame("tx", buf, len, NULL, NULL);
	}
	return link_radio_transport.send(buf, len);
}

static int recv(void *buf, size_t size, k_timeout_t timeout, int16_t *rssi_dbm, int8_t *snr_db)
{
	int len = link_radio_transport.recv(buf, size, timeout, rssi_dbm, snr_db);

	if (len >= 0 && atomic_get(&mode) != TRACE_OFF) {
		log_frame("rx", buf, len, rssi_dbm, snr_db);
	}
	return len;
}

static int set_profile(enum link_profile p)
{
	if (atomic_get(&mode) != TRACE_OFF) {
		LOG_INF("-- %s profile", p == LINK_PROFILE_PAIR ? "pairing" : "link");
	}
	return link_radio_transport.set_profile(p);
}

static enum link_profile profile(void)
{
	return link_radio_transport.profile();
}

const struct link_transport trace_transport = {
	.send = send,
	.recv = recv,
	.set_profile = set_profile,
	.profile = profile,
};

void trace_set_mode(enum trace_mode m)
{
	atomic_set(&mode, m);
}

enum trace_mode trace_mode(void)
{
	return atomic_get(&mode);
}
