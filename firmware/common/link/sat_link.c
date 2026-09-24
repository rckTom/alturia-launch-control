/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "lsec_store.h"
#include "sat_link.h"

LOG_MODULE_REGISTER(sat_link, LOG_LEVEL_INF);

#define LINK_STACK_SIZE 6144
#define LINK_PRIORITY   5
/* Receive window; also the cadence of lsec's timers. */
#define RX_WINDOW K_MSEC(50)

static struct lsec_sat sat;
/* Guards sat. Recursive, so event handlers may call back in. */
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

int sat_link_replay_last(void)
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
	if (idx == 0 && lsec_sat_load(&sat, p) == 0) {
		LOG_INF("paired as slot %u of network %08x", p->slot, p->net_id);
	}
}

/* Satellite identity: the hwinfo device id folded to 64 bits. */
static uint64_t read_uid(void)
{
	uint8_t id[16] = {0};
	ssize_t len = hwinfo_get_device_id(id, sizeof(id));
	uint64_t uid = 0;

	for (ssize_t i = 0; i < len; i++) {
		uid ^= (uint64_t)id[i] << (8 * (i % 8));
	}

	return uid;
}

/* Pairing happens on its own short-range radio profile. */
static void update_profile(void)
{
	enum link_profile want =
		lsec_sat_pairing_active(&sat) ? LINK_PROFILE_PAIR : LINK_PROFILE_LINK;

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
			lsec_sat_rx(&sat, buf, len, k_uptime_get());
		}
		lsec_sat_poll(&sat, k_uptime_get());
		update_profile();

		k_mutex_unlock(&lock);
	}
}

int sat_link_init(link_event_cb cb, const struct link_transport *transport)
{
	const struct lsec_io io = {
		.send = io_send,
		.event = io_event,
		.store = lsec_store_save,
	};
	uint64_t uid = read_uid();
	int ret;

	event_cb = cb;
	tp = transport;

	ret = lsec_sat_init(&sat, &io, uid);
	if (ret) {
		return ret;
	}

	ret = lsec_store_load(NULL, load_one, NULL);
	if (ret < 0) {
		return ret;
	}

	LOG_INF("satellite %016llx, %s", uid,
		lsec_sat_paired(&sat) ? "paired" : "not paired, link closed");

	k_thread_create(&link_thread, link_stack, K_THREAD_STACK_SIZEOF(link_stack), link_main,
			NULL, NULL, NULL, LINK_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&link_thread, "sat_link");

	return 0;
}

int sat_link_send(const void *data, size_t len)
{
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	ret = lsec_sat_send(&sat, data, len);
	k_mutex_unlock(&lock);

	return ret;
}

int sat_link_pair_start(void)
{
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	ret = lsec_sat_pair_start(&sat, k_uptime_get());
	update_profile();
	k_mutex_unlock(&lock);

	return ret;
}

void sat_link_pair_confirm(bool accept)
{
	k_mutex_lock(&lock, K_FOREVER);
	lsec_sat_pair_confirm(&sat, accept, k_uptime_get());
	k_mutex_unlock(&lock);
}

int sat_link_unpair(void)
{
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	ret = lsec_sat_unpair(&sat);
	k_mutex_unlock(&lock);

	return ret;
}

void sat_link_state(struct sat_link_state *out)
{
	k_mutex_lock(&lock, K_FOREVER);
	*out = (struct sat_link_state){
		.uid = sat.uid,
		.slot = sat.paired ? sat.pairing.slot : 0,
		.paired = sat.paired,
		.joined = sat.joined,
		.pairing = lsec_sat_pairing_active(&sat),
		.rssi_dbm = last_rssi,
		.snr_db = last_snr,
		.stats = sat.stats,
	};
	k_mutex_unlock(&lock);
}
