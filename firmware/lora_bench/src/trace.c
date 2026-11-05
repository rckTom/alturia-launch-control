/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * One line per frame, for example:
 *
 *   rx HELLO        1 -> 0   net 3a41, 5 B, -45 dBm 9 dB
 *   tx JOIN_REQ     0 -> 1   net 3a41, 13 B
 *   tx DATA         0 -> ff  net 3a41 kid 2 ctr 17, 49 B
 *   rx PAIR_ADV     uid 00001c6fa68fcba4, 9 B, -40 dBm 10 dB
 *   rx DATA         8 -> 0   net 3a41 kid 0 ctr 41, 31 B, -45 dBm 9 dB, +44.1 ms
 *
 * The last field is the time since the last poll, which is where the reply
 * slots count from. "lbt busy" lines are listens that found someone on
 * the air.
 *
 * Addresses: 0 is the controller, ff the group, anything else a
 * satellite's slot.
 */

#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "link_access.h"
#include "lsec_internal.h"
#include "radio.h"
#include "trace.h"

LOG_MODULE_REGISTER(trace, LOG_LEVEL_INF);

static atomic_t mode = ATOMIC_INIT(TRACE_OFF);
static atomic_t jam_slot;

/*
 * A frame that is no lsec frame (version 0), about 200 ms on air. It goes
 * out some milliseconds after the poll, once the radio has turned around,
 * so it covers the slots after the first: test with phantoms ahead.
 */
#define JAM_LEN 255

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
	case KIND_POLL:
		return "POLL";
	default:
		return NULL;
	}
}

/* When the last poll was sent (controller) or received (satellite). */
static int64_t poll_us = INT64_MIN / 2;

/* Append to a line being built; never overflows. */
#define APPEND(line, pos, ...)                                                             \
	((pos) += snprintk((line) + (pos), (pos) < sizeof(line) ? sizeof(line) - (pos) : 0,    \
			   __VA_ARGS__))

/*
 * @p rx is NULL for a transmitted frame; @p t_us is when it was sent or
 * received, and @p took_us how long sending blocked.
 */
static void log_frame(const char *dir, const uint8_t *buf, size_t len,
		      const struct link_rx_info *rx, int64_t t_us, int64_t took_us)
{
	int kind = lsec_frame_kind(buf, len);
	const char *name = kind_name(kind);
	int64_t since_poll = t_us - poll_us;
	char line[112];
	size_t pos = 0;
	struct lsec_hdr h;

	if (!name) {
		/* Wrong version, or not an lsec frame at all. */
		APPEND(line, pos, "%s ?%02x", dir, len ? buf[0] : 0);
	} else if (KIND_IS_PAIR(kind)) {
		uint64_t uid = len >= PAIR_HDR ? get_le64(&buf[1]) : 0;

		APPEND(line, pos, "%s %-12s uid %016llx", dir, name, uid);
	} else if (lsec_parse_hdr(buf, len, &h) != 0) {
		APPEND(line, pos, "%s %-12s truncated", dir, name);
	} else {
		APPEND(line, pos, "%s %-12s %x -> %-2x  net %04x", dir, name, h.src, h.dst,
		       h.net_tag);
		if (KIND_IS_SEALED(kind)) {
			APPEND(line, pos, " kid %u ctr %u", h.kid, h.ctr);
		}
	}

	APPEND(line, pos, ", %zu B", len);
	if (rx) {
		APPEND(line, pos, ", %d dBm %d dB", rx->rssi_dbm, rx->snr_db);
	} else {
		APPEND(line, pos, ", sent in %lld.%01lld ms", took_us / 1000, (took_us % 1000) / 100);
	}
	/* Where in the poll cycle it fell. */
	if (kind != KIND_POLL && since_poll >= 0 && since_poll < 2000000) {
		APPEND(line, pos, ", +%lld.%01lld ms", since_poll / 1000, (since_poll % 1000) / 100);
	}

	LOG_INF("%s", line);

	if (atomic_get(&mode) == TRACE_HEX) {
		LOG_HEXDUMP_INF(buf, len, "");
	}
}

static int send(const void *buf, size_t len)
{
	int64_t start = link_now_us();
	int ret = link_radio_transport.send(buf, len);

	if (atomic_get(&mode) != TRACE_OFF) {
		log_frame("tx", buf, len, NULL, start, link_now_us() - start);
	}
	/* Slots count from the end of the poll. */
	if (lsec_frame_kind(buf, len) == KIND_POLL) {
		poll_us = link_now_us();

		if (atomic_get(&jam_slot)) {
			static const uint8_t jam[JAM_LEN];

			(void)link_radio_transport.send(jam, sizeof(jam));
			LOG_INF("jammed the reply slots");
		}
	}
	return ret;
}

static int recv(void *buf, size_t size, k_timeout_t timeout, struct link_rx_info *info)
{
	int len = link_radio_transport.recv(buf, size, timeout, info);

	if (len < 0) {
		return len;
	}
	if (lsec_frame_kind(buf, len) == KIND_POLL) {
		poll_us = info->time_us;
	}
	if (atomic_get(&mode) != TRACE_OFF) {
		log_frame("rx", buf, len, info, info->time_us, 0);
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

static int channel_clear(uint32_t listen_us, int16_t *peak_dbm)
{
	int16_t peak = 0;
	int ret = link_radio_transport.channel_clear(listen_us, &peak);

	if (ret == 0 && atomic_get(&mode) != TRACE_OFF) {
		LOG_INF("lbt busy: %d dBm, floor %d dBm", peak, radio_noise_floor());
	}
	if (peak_dbm) {
		*peak_dbm = peak;
	}
	return ret;
}

static uint32_t airtime_ms(size_t len)
{
	return link_radio_transport.airtime_ms(len);
}

const struct link_transport trace_transport = {
	.send = send,
	.recv = recv,
	.set_profile = set_profile,
	.profile = profile,
	.channel_clear = channel_clear,
	.airtime_ms = airtime_ms,
};

void trace_set_jam_slot(bool on)
{
	atomic_set(&jam_slot, on);
}

void trace_set_mode(enum trace_mode m)
{
	atomic_set(&mode, m);
}

enum trace_mode trace_mode(void)
{
	return atomic_get(&mode);
}
