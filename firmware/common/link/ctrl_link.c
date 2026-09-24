/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ctrl_link.h"
#include "lsec_store.h"

LOG_MODULE_REGISTER(ctrl_link, LOG_LEVEL_INF);

#define LINK_STACK_SIZE 6144
#define LINK_PRIORITY   5
/* Receive window; also the cadence of lsec's timers. */
#define RX_WINDOW K_MSEC(50)

static struct lsec_ctrl ctrl;
/* Guards ctrl. Recursive, so event handlers may call back in. */
static K_MUTEX_DEFINE(lock);
static link_event_cb event_cb;
static const struct link_transport *tp;
static int16_t last_rssi;
static int8_t last_snr;

static K_THREAD_STACK_DEFINE(link_stack, LINK_STACK_SIZE);
static struct k_thread link_thread;

#ifdef LINK_TEST_HOOKS
#include "lsec_internal.h"

/* The last sealed DATA frame sent, kept for replay tests. */
static uint8_t last_data[LSEC_MAX_FRAME];
static size_t last_data_len;

int ctrl_link_replay_last(void)
{
	if (last_data_len == 0) {
		return -ENOENT;
	}

	return tp->send(last_data, last_data_len);
}
#endif

static void io_send(void *user, const uint8_t *buf, size_t len)
{
	int ret;

#ifdef LINK_TEST_HOOKS
	if (lsec_frame_kind(buf, len) == KIND_DATA) {
		memcpy(last_data, buf, len);
		last_data_len = len;
	}
#endif

	ret = tp->send(buf, len);

	if (ret) {
		LOG_ERR("send: %d", ret);
	}
}

static void io_event(void *user, const struct lsec_event *ev)
{
	if (event_cb) {
		event_cb(ev);
	}
}

static void load_one(uint8_t idx, const struct lsec_pairing *p, void *user)
{
	if (idx != 0 && lsec_ctrl_load(&ctrl, p) == 0) {
		LOG_INF("slot %u: satellite %016llx", p->slot, p->sat_uid);
	}
}

/* Scanning and pairing happen on their own short-range radio profile. */
static void update_profile(void)
{
	bool pairing = ctrl.scanning || lsec_ctrl_pairing_active(&ctrl);
	enum link_profile want = pairing ? LINK_PROFILE_PAIR : LINK_PROFILE_LINK;

	if (tp->profile() != want) {
		tp->set_profile(want);
	}
}

static void link_main(void *p1, void *p2, void *p3)
{
	static uint8_t buf[LSEC_MAX_FRAME];

	while (true) {
		int16_t rssi = 0;
		int8_t snr = 0;
		int len = tp->recv(buf, sizeof(buf), RX_WINDOW, &rssi, &snr);

		k_mutex_lock(&lock, K_FOREVER);

		if (len > 0) {
			last_rssi = rssi;
			last_snr = snr;
			lsec_ctrl_rx(&ctrl, buf, len, rssi, k_uptime_get());
		}
		lsec_ctrl_poll(&ctrl, k_uptime_get());
		update_profile();

		k_mutex_unlock(&lock);
	}
}

int ctrl_link_init(link_event_cb cb, const struct link_transport *transport)
{
	const struct lsec_io io = {
		.send = io_send,
		.event = io_event,
		.store = lsec_store_save,
	};
	uint32_t net_id;
	int ret;

	event_cb = cb;
	tp = transport;

	/* The network id is needed before the pairings can be loaded. */
	ret = lsec_store_load(&net_id, NULL, NULL);
	if (ret < 0) {
		return ret;
	}

	ret = lsec_ctrl_init(&ctrl, &io, net_id);
	if (ret) {
		return ret;
	}

	ret = lsec_store_load(&net_id, load_one, NULL);
	if (ret < 0) {
		return ret;
	}

	LOG_INF("controller, network %08x, %d paired", net_id, ret);

	k_thread_create(&link_thread, link_stack, K_THREAD_STACK_SIZEOF(link_stack), link_main,
			NULL, NULL, NULL, LINK_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&link_thread, "ctrl_link");

	return 0;
}

int ctrl_link_send(uint8_t dst, const void *data, size_t len)
{
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	ret = lsec_ctrl_send(&ctrl, dst, data, len);
	k_mutex_unlock(&lock);

	return ret;
}

void ctrl_link_scan(bool on)
{
	k_mutex_lock(&lock, K_FOREVER);
	lsec_ctrl_scan(&ctrl, on);
	update_profile();
	k_mutex_unlock(&lock);
}

int ctrl_link_pair_start(uint64_t uid, uint8_t slot)
{
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	lsec_ctrl_scan(&ctrl, false);
	ret = lsec_ctrl_pair_start(&ctrl, uid, slot, k_uptime_get());
	update_profile();
	k_mutex_unlock(&lock);

	return ret;
}

void ctrl_link_pair_confirm(bool accept)
{
	k_mutex_lock(&lock, K_FOREVER);
	lsec_ctrl_pair_confirm(&ctrl, accept, k_uptime_get());
	k_mutex_unlock(&lock);
}

int ctrl_link_forget(uint8_t slot)
{
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	ret = lsec_ctrl_forget(&ctrl, slot, k_uptime_get());
	k_mutex_unlock(&lock);

	return ret;
}

bool ctrl_link_joined(uint8_t slot)
{
	bool joined;

	k_mutex_lock(&lock, K_FOREVER);
	joined = lsec_ctrl_joined(&ctrl, slot);
	k_mutex_unlock(&lock);

	return joined;
}

uint64_t ctrl_link_paired_uid(uint8_t slot)
{
	const struct lsec_pairing *p;
	uint64_t uid;

	k_mutex_lock(&lock, K_FOREVER);
	p = lsec_ctrl_pairing(&ctrl, slot);
	uid = p ? p->sat_uid : 0;
	k_mutex_unlock(&lock);

	return uid;
}

void ctrl_link_state(struct ctrl_link_state *out)
{
	k_mutex_lock(&lock, K_FOREVER);
	*out = (struct ctrl_link_state){
		.net_id = ctrl.net_id,
		.scanning = ctrl.scanning,
		.pairing = lsec_ctrl_pairing_active(&ctrl),
		.rssi_dbm = last_rssi,
		.snr_db = last_snr,
		.stats = ctrl.stats,
	};
	k_mutex_unlock(&lock);
}
