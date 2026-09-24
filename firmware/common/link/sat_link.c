/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "lsec_internal.h"
#include "lsec_store.h"
#include "sat_link.h"

LOG_MODULE_REGISTER(sat_link, LOG_LEVEL_INF);

#define LINK_STACK_SIZE 6144
#define LINK_PRIORITY   5
/* Receive window; also the cadence of lsec's timers. */
#define RX_WINDOW K_MSEC(50)

/*
 * How long frames outside a slot may wait for a clear channel. An answer
 * to the controller comes right after its request, into the gap the
 * controller leaves for it; HELLO and PAIR_ADV are repeated anyway.
 */
#define ANSWER_DEFER_US      (10 * 1000)
#define UNSOLICITED_DEFER_US (200 * 1000)

static struct lsec_sat sat;
/* Guards sat and everything below. Recursive, so event handlers may call back in. */
static K_MUTEX_DEFINE(lock);
static link_event_cb event_cb;
static const struct link_transport *tp;
static int16_t last_rssi;
static int8_t last_snr;

/* The answer to the poll being handled, waiting for its slot. */
static struct {
	bool pending;
	int64_t at_us;       /* slot start */
	uint8_t slot_ms;
	size_t len;
	uint8_t data[LSEC_MAX_PAYLOAD];
} reply;

/* Reception time of the frame lsec is handling. */
static int64_t rx_time_us;
/* io_send is sending the reply, which has had its listen already. */
static bool in_slot;
static struct sat_link_state counters;

static K_THREAD_STACK_DEFINE(link_stack, LINK_STACK_SIZE);
static struct k_thread link_thread;

#ifdef LINK_TEST_HOOKS
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

/* Answers go straight into the controller's gap; the rest waits its turn. */
static bool is_answer(int kind)
{
	switch (kind) {
	case KIND_HELLO:
	case KIND_PAIR_ADV:
	case KIND_DATA:
		return false;
	default:
		return true;
	}
}

static void io_send(void *user, const uint8_t *buf, size_t len)
{
	int kind = lsec_frame_kind(buf, len);
	int ret;

#ifdef LINK_TEST_HOOKS
	if (kind == KIND_DATA) {
		memcpy(last_data, buf, len);
		last_data_len = len;
	}
#endif

	if (!in_slot) {
		bool answer = is_answer(kind);

		if (link_lbt_wait(tp, answer ? ANSWER_DEFER_US : UNSOLICITED_DEFER_US, false,
				  !answer, &counters.lbt) == LINK_LBT_GIVE_UP) {
			/* lsec repeats what matters: HELLO, ADV, and the controller's requests. */
			LOG_DBG("channel busy, dropped kind %d", kind);
			return;
		}
	}

	ret = tp->send(buf, len);

	if (ret) {
		LOG_ERR("send: %d", ret);
	}
}

static void io_event(void *user, const struct lsec_event *ev)
{
	if (ev->type == LSEC_EV_POLL) {
		/* Queue an empty answer; the handler may replace it. */
		reply.pending = true;
		reply.at_us = link_slot_start_us(rx_time_us, ev->rank, ev->slot_ms);
		reply.slot_ms = ev->slot_ms;
		reply.len = 0;
		counters.polls++;
		counters.last_poll_us = rx_time_us;
	}

	if (event_cb) {
		event_cb(ev);
	}
}

/* The slot has come: listen, then send the reply or let the slot pass. */
static void send_reply(void)
{
	int64_t late_us = link_now_us() - reply.at_us;

	reply.pending = false;

	if (late_us > LINK_SLOT_GUARD_MS * 1000 / 2) {
		counters.slot_late++;
		LOG_WRN("missed slot by %lld us", late_us);
		return;
	}

	if (LINK_LBT_LISTEN_US && tp->channel_clear &&
	    tp->channel_clear(LINK_LBT_LISTEN_US, NULL) == 0) {
		/* Someone is on the air: sending now would ruin both frames. */
		counters.slot_busy++;
		counters.lbt.busy++;
		return;
	}

	counters.last_reply_us = link_now_us();
	in_slot = true;
	if (lsec_sat_send(&sat, reply.data, reply.len) == 0) {
		counters.replies++;
	}
	in_slot = false;
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
		struct link_rx_info info = {0};
		k_timeout_t window = RX_WINDOW;
		int len;

		/* Waiting for a slot: keep receiving, but only until it starts. */
		k_mutex_lock(&lock, K_FOREVER);
		if (reply.pending) {
			int64_t wait_us = reply.at_us - link_now_us();

			window = wait_us > 0 ? K_USEC(wait_us) : K_NO_WAIT;
		}
		k_mutex_unlock(&lock);

		len = tp->recv(buf, sizeof(buf), window, &info);

		k_mutex_lock(&lock, K_FOREVER);

		if (len > 0) {
			last_rssi = info.rssi_dbm;
			last_snr = info.snr_db;
			rx_time_us = info.time_us;
			lsec_sat_rx(&sat, buf, len, k_uptime_get());
		}

		if (reply.pending && link_now_us() >= reply.at_us) {
			send_reply();
		}

		/* lsec's own frames (HELLO) would land in someone's slot. */
		if (!reply.pending) {
			lsec_sat_poll(&sat, k_uptime_get());
		}
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
	uint32_t seed;
	int ret;

	event_cb = cb;
	tp = transport;

	ret = lsec_sat_init(&sat, &io, uid);
	if (ret) {
		return ret;
	}

	/* Satellites that power up together must not back off in step. */
	if (lsec_random(&seed, sizeof(seed)) == 0) {
		link_sched_seed(seed);
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

int sat_link_reply(const void *data, size_t len)
{
	int ret = 0;

	k_mutex_lock(&lock, K_FOREVER);

	if (!reply.pending) {
		ret = -EAGAIN;
	} else if (len > sizeof(reply.data)) {
		ret = -EMSGSIZE;
	} else {
		int need = link_slot_ms(tp->airtime_ms(len + LSEC_OVERHEAD));

		if (need < 0 || need > reply.slot_ms) {
			ret = -EMSGSIZE;
		} else {
			memcpy(reply.data, data, len);
			reply.len = len;
		}
	}

	k_mutex_unlock(&lock);
	return ret;
}

int sat_link_send(const void *data, size_t len)
{
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	ret = reply.pending ? -EBUSY : lsec_sat_send(&sat, data, len);
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
	*out = counters;
	out->uid = sat.uid;
	out->slot = sat.paired ? sat.pairing.slot : 0;
	out->paired = sat.paired;
	out->joined = sat.joined;
	out->pairing = lsec_sat_pairing_active(&sat);
	out->rssi_dbm = last_rssi;
	out->snr_db = last_snr;
	out->stats = sat.stats;
	k_mutex_unlock(&lock);
}
