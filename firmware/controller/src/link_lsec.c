/*
 * link.h on the secure link (../common/link/ctrl_link.c). Commands go out
 * in one group-sealed poll per tick, and each satellite's status comes
 * back sealed in its reply slot; neither is sent or accepted without a
 * joined session.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ctrl_link.h"
#include "ignlog.h"
#include "link.h"
#include "sat_wire.h"
#include "pads.h"

#ifdef CONFIG_APP_SAT_SIM
#include "sat_sim.h"
#else
#include "radio.h"
#endif

LOG_MODULE_REGISTER(link, LOG_LEVEL_INF);

#define SLOT(sat) ((uint8_t)((sat) + 1))

static K_MUTEX_DEFINE(lock);
static struct sat_status rx_status[NUM_SATS];
static int64_t rx_time[NUM_SATS] = {[0 ... NUM_SATS - 1] = -1};

K_MSGQ_DEFINE(pair_events, sizeof(struct link_pair_event), 8, 4);

static void queue_pair_event(const struct lsec_event *ev)
{
	struct link_pair_event pe = {
		.type = ev->type,
		.sat = ev->slot - 1,
		.uid = ev->uid,
		.rssi_dbm = ev->rssi_dbm,
		.reason = ev->reason,
	};

	memcpy(pe.sas, ev->sas, sizeof(pe.sas));
	if (k_msgq_put(&pair_events, &pe, K_NO_WAIT)) {
		LOG_WRN("pairing event %d dropped", ev->type);
	}
}

/* Runs on the link thread. */
static void on_event(const struct lsec_event *ev)
{
	struct sat_status st;
	int sat = ev->slot - 1;

	switch (ev->type) {
	case LSEC_EV_DATA:
		if (sat < 0 || sat >= NUM_SATS || ev->group) {
			return;
		}
		/* Log replies go to the ignition log download (ignlog.c). */
		if (wire_reply_kind(ev->data, ev->len) != WIRE_REPLY_STATUS) {
			ignlog_rx(sat, ev->data, ev->len);
			return;
		}
		/* A short status updates the last one; the full-only fields stay. */
		k_mutex_lock(&lock, K_FOREVER);
		st = rx_status[sat];
		if (wire_decode_status(ev->data, ev->len, &st) == 0) {
			rx_status[sat] = st;
			rx_time[sat] = k_uptime_get();
		}
		k_mutex_unlock(&lock);
		break;
	case LSEC_EV_JOINED:
		LOG_INF("satellite %c: session up", sat_name(sat));
		break;
	case LSEC_EV_LEFT:
		LOG_WRN("satellite %c: no reply for %u ms, session dropped", sat_name(sat),
			LSEC_PEER_TIMEOUT_MS);
		break;
	case LSEC_EV_POLL:
		break;   /* satellite side only */
	default:
		queue_pair_event(ev);
		break;
	}
}

int link_init(void)
{
#ifdef CONFIG_APP_SAT_SIM
	return ctrl_link_init(on_event, sat_sim_transport());
#else
	/* The controller's LoRa module has no external front-end. */
	int ret = radio_init(NULL);

	if (ret) {
		LOG_ERR("radio init: %d", ret);
		return ret;
	}
	return ctrl_link_init(on_event, &link_radio_transport);
#endif
}

int link_poll(const struct link_cmd cmds[NUM_SATS], bool full)
{
	uint8_t buf[WIRE_POLL_MAX_LEN(NUM_SATS)];
	/* Satellite n is slot n + 1, which is bit n of the link's mask. */
	uint8_t mask = ctrl_link_joined_mask() & BIT_MASK(NUM_SATS);
	size_t len;

	/* Without a session a command is lost, like any other lost packet. */
	if (mask == 0) {
		return -ENOTCONN;
	}

	len = wire_encode_poll(cmds, NUM_SATS, mask, full, buf);
	return ctrl_link_poll(mask, buf, len, full ? WIRE_STATUS_LEN : WIRE_STATUS_SHORT_LEN);
}

int link_log_poll(int sat, const struct wire_log_req *req)
{
	uint8_t buf[WIRE_LOG_POLL_LEN];
	size_t len = wire_encode_log_poll(req, buf);
	/* The index, or a chunk; a chunk request may be answered with the index. */
	size_t reply = MAX(WIRE_LOG_CHUNK_LEN, WIRE_LOG_INDEX_LEN(WIRE_LOG_INDEX_MAX));

	if (sat < 0 || sat >= NUM_SATS || !ctrl_link_joined(SLOT(sat))) {
		return -ENOTCONN;
	}
	return ctrl_link_poll(BIT(sat), buf, len, reply);
}

int64_t link_next_poll_ms(uint32_t period_ms)
{
	return ctrl_link_next_poll_ms(period_ms);
}

void link_channel(uint8_t *util_pct, uint8_t *other_nets)
{
	struct ctrl_link_state ls;

	ctrl_link_state(&ls);
	*util_pct = ls.util_pct;
	*other_nets = ls.foreign_nets;
}

int64_t link_last_status(int sat, struct sat_status *out)
{
	int64_t t;

	k_mutex_lock(&lock, K_FOREVER);
	*out = rx_status[sat];
	t = rx_time[sat];
	k_mutex_unlock(&lock);
	return t;
}

void link_scan(bool on)
{
	ctrl_link_scan(on);
}

int link_pair(int sat, uint64_t uid)
{
	return ctrl_link_pair_start(uid, SLOT(sat));
}

void link_pair_confirm(bool accept)
{
	ctrl_link_pair_confirm(accept);
}

int link_forget(int sat)
{
	int ret = ctrl_link_forget(SLOT(sat));

	k_mutex_lock(&lock, K_FOREVER);
	rx_time[sat] = -1;
	k_mutex_unlock(&lock);
	return ret;
}

uint64_t link_paired_uid(int sat)
{
	return ctrl_link_paired_uid(SLOT(sat));
}

bool link_joined(int sat)
{
	return ctrl_link_joined(SLOT(sat));
}

bool link_pair_event(struct link_pair_event *ev)
{
	return k_msgq_get(&pair_events, ev, K_NO_WAIT) == 0;
}
