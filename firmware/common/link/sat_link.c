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

static struct lsec_sat sat;
/* Guards sat and everything below. Recursive, so event handlers may call back in. */
static K_MUTEX_DEFINE(lock);
static link_event_cb event_cb;
static const struct link_transport *tp;
static int16_t last_rssi;
static int8_t last_snr;
/* How busy the channel is, as this satellite hears it. */
static struct link_util util;
/*
 * Whether pairing is on, readable without the lock: the lock is held across
 * a send, and a send waits for the radio's TX-done work item, so a work
 * item that waited for the lock would stall the system work queue for the
 * driver's 15 s TX timeout.
 */
static atomic_t pairing_on;
/* Whether a session with the controller is up, likewise. */
static atomic_t joined_on;

/*
 * Pairing gestures from other threads, carried out by the link thread: they
 * end in crypto, a flash write and a send, which need the link thread's
 * stack (the input thread's 1 KB overflowed on a confirm).
 */
#define REQ_PAIR_START BIT(0)
#define REQ_ACCEPT     BIT(1)
#define REQ_REJECT     BIT(2)
static atomic_t requests;

/* The answer to the poll being handled, waiting for its slot. */
static struct {
	bool pending;
	int64_t at_us;       /* slot start (LINK_REPLY_CSMA: the window's) */
	int64_t end_us;      /* LINK_REPLY_CSMA: the reply window's end */
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

	/*
	 * An answer to the controller goes right after its request, into the
	 * gap the controller leaves for it; HELLO and PAIR_ADV are repeated
	 * anyway, so they back off further on a busy channel.
	 */
	if (!in_slot && link_csma_wait(tp, is_answer(kind) ? &link_csma_answer
							   : &link_csma_unsolicited,
				       &util, &lock, &counters.csma) == LINK_CSMA_GIVE_UP) {
		/* lsec repeats what matters: HELLO, ADV, and the controller's requests. */
		LOG_DBG("channel busy, dropped kind %d", kind);
		return;
	}

	int64_t start = link_now_us();

	ret = tp->send(buf, len);

	if (ret) {
		LOG_ERR("send: %d", ret);
	}
	link_util_add(&util, start, link_now_us());
}

static void io_event(void *user, const struct lsec_event *ev)
{
	if (ev->type == LSEC_EV_POLL) {
		/* Queue an empty answer; the handler may replace it. */
		reply.pending = true;
		if (IS_ENABLED(CONFIG_LINK_REPLY_CSMA)) {
			reply.at_us = link_slot_start_us(rx_time_us, 0, ev->slot_ms);
			reply.end_us = link_replies_end_us(rx_time_us, ev->count, ev->slot_ms);
		} else {
			reply.at_us = link_slot_start_us(rx_time_us, ev->rank, ev->slot_ms);
		}
		reply.slot_ms = ev->slot_ms;
		reply.len = 0;
		counters.polls++;
		counters.last_poll_us = rx_time_us;
	}

	if (event_cb) {
		event_cb(ev);
	}
}

/*
 * LINK_REPLY_CSMA: contend for the rest of the reply window. The reply has
 * to start one slot before the window ends; the backoff window takes about
 * half of what is left, the rest is for waiting out a busy channel.
 */
static enum link_csma_res reply_in_window(void)
{
	int64_t room = reply.end_us - (int64_t)reply.slot_ms * 1000 - link_now_us();
	struct link_csma_class c = {.force = false};
	int64_t slots = room / 2 / LINK_CSMA_SLOT_US;

	if (room < 0) {
		return LINK_CSMA_GIVE_UP;
	}
	while (c.cw_min < 10 && (2 << c.cw_min) <= slots + 1) {
		c.cw_min++;
	}
	c.cw_max = c.cw_min;
	c.max_defer_us = room - ((1 << c.cw_min) - 1) * LINK_CSMA_SLOT_US;

	return link_csma_wait(tp, &c, &util, &lock, &counters.csma);
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

	/* One sense at the slot's start, no backoff: the slot is ours. */
	if ((IS_ENABLED(CONFIG_LINK_REPLY_CSMA)
		     ? reply_in_window()
		     : link_csma_wait(tp, &link_csma_slot, &util, &lock, &counters.csma)) ==
	    LINK_CSMA_GIVE_UP) {
		/* Someone is on the air: sending now would ruin both frames. */
		counters.slot_busy++;
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
	bool pairing = lsec_sat_pairing_active(&sat);
	enum link_profile want = pairing ? LINK_PROFILE_PAIR : LINK_PROFILE_LINK;

	/* A start that is still queued counts, or pair_ui would stop blinking. */
	atomic_set(&pairing_on, pairing || (atomic_get(&requests) & REQ_PAIR_START));
	atomic_set(&joined_on, sat.joined);

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
			link_util_add(&util, info.time_us - (int64_t)tp->airtime_ms(len) * 1000,
				      info.time_us);
			lsec_sat_rx(&sat, buf, len, k_uptime_get());
		}

		if (reply.pending && link_now_us() >= reply.at_us) {
			send_reply();
		}

		atomic_val_t req = atomic_clear(&requests);

		if (req & REQ_PAIR_START) {
			(void)lsec_sat_pair_start(&sat, k_uptime_get());
		}
		if (req & (REQ_ACCEPT | REQ_REJECT)) {
			lsec_sat_pair_confirm(&sat, !(req & REQ_REJECT), k_uptime_get());
		}

		/* lsec's own frames (HELLO) would land in someone's slot. */
		if (!reply.pending) {
			lsec_sat_poll(&sat, k_uptime_get());
		}
		update_profile();

		k_mutex_unlock(&lock);
	}
}

static int store(void *user, uint8_t slot, const struct lsec_pairing *p)
{
	int ret = lsec_store_save(user, slot, p);

	if (ret) {
		LOG_ERR("pairing %s slot %u: %d", p ? "save" : "delete", slot, ret);
	}
	return ret;
}

int sat_link_init(link_event_cb cb, const struct link_transport *transport)
{
	const struct lsec_io io = {
		.send = io_send,
		.event = io_event,
		.store = store,
	};
	uint64_t uid = read_uid();
	uint32_t seed;
	int ret;

	event_cb = cb;
	tp = transport;
	link_util_init(&util, link_now_us());

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

static void request(atomic_val_t req)
{
	atomic_or(&requests, req);
	if (k_current_get() != &link_thread && tp->wake) {
		tp->wake();
	}
}

int sat_link_pair_start(void)
{
	atomic_set(&pairing_on, true);
	request(REQ_PAIR_START);

	return 0;
}

void sat_link_pair_confirm(bool accept)
{
	request(accept ? REQ_ACCEPT : REQ_REJECT);
}

int sat_link_unpair(void)
{
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	ret = lsec_sat_unpair(&sat);
	k_mutex_unlock(&lock);

	return ret;
}

bool sat_link_pairing(void)
{
	return atomic_get(&pairing_on);
}

bool sat_link_joined(void)
{
	return atomic_get(&joined_on);
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
	out->util_pct = link_util_pct(&util, link_now_us());
	k_mutex_unlock(&lock);
}
