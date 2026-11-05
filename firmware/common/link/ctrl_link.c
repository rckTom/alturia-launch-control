/* SPDX-License-Identifier: GPL-3.0-only */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ctrl_link.h"
#include "lsec_internal.h"
#include "lsec_store.h"

LOG_MODULE_REGISTER(ctrl_link, LOG_LEVEL_INF);

#define LINK_STACK_SIZE 6144
#define LINK_PRIORITY   5
/*
 * Receive window when there is nothing to send. Frames and polls queued by
 * other threads wake the link thread through the transport, so this only
 * paces lsec's timers, which count in seconds, and lets the CPU sleep. A
 * transport that cannot wake it gets the short window, which is also how
 * long a newly queued poll can wait before the thread notices it.
 */
#define RX_WINDOW_US       100000
#define RX_WINDOW_SHORT_US 5000

#define TX_QUEUE_DEPTH   12
#define POLL_QUEUE_DEPTH 4

/* The longest answer a frame can draw: JOIN_RSP on the link, PAIR_RSP while pairing. */
#define LINK_ANSWER_LEN (LINK_HDR + NONCE_JOIN + TAG_BYTES)
#define PAIR_ANSWER_LEN (PAIR_HDR + PK_BYTES + CHECK_BYTES)
/* How long the satellite may wait for a clear channel before it answers. */
#define ANSWER_DEFER_MS 10

struct tx_frame {
	uint8_t len;
	bool poll;
	uint8_t count;     /* poll: replies expected */
	uint8_t slot_ms;   /* poll: length of each */
	uint8_t buf[LSEC_MAX_FRAME];
};

struct poll_req {
	uint8_t mask;
	uint8_t slot_ms;
	uint8_t len;
	uint8_t data[LSEC_MAX_POLL_PAYLOAD];
};

K_MSGQ_DEFINE(tx_q, sizeof(struct tx_frame), TX_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(poll_q, sizeof(struct poll_req), POLL_QUEUE_DEPTH, 4);

static struct lsec_ctrl ctrl;
/* Guards ctrl and everything below. Recursive, so event handlers may call back in. */
static K_MUTEX_DEFINE(lock);
static link_event_cb event_cb;
static const struct link_transport *tp;
static int16_t last_rssi;
static int8_t last_snr;
static struct ctrl_link_state counters;

/* io_send is queueing the poll the link thread is sealing. */
static const struct poll_req *sealing_poll;

/* Other networks' air time, as far as we hear it. Written under lock, by the link thread. */
static struct link_foreign foreign;
/* How busy the channel is, every network counted. Guarded by lock. */
static struct link_util util;

/* Our last poll: when it started and how long its cycle was. Guarded by lock. */
static int64_t poll_start_us;
static uint32_t poll_len_us;
/* Polls queued, and polls sent or dropped: equal when none is waiting. */
static uint32_t polls_queued;
static uint32_t polls_done;

static K_THREAD_STACK_DEFINE(link_stack, LINK_STACK_SIZE);
static struct k_thread link_thread;
/* A wake is on its way to the link thread's recv(). */
static atomic_t woken;

/* Another thread queued work: get the link thread out of recv(). */
static void wake_link(void)
{
	if (k_current_get() != &link_thread && tp->wake && atomic_cas(&woken, 0, 1)) {
		tp->wake();
	}
}

static void enqueue(const uint8_t *buf, size_t len)
{
	struct tx_frame f = {.len = len};

	memcpy(f.buf, buf, len);
	if (sealing_poll) {
		f.poll = true;
		f.count = __builtin_popcount(sealing_poll->mask);
		f.slot_ms = sealing_poll->slot_ms;
	}

	if (k_msgq_put(&tx_q, &f, K_NO_WAIT)) {
		/* lsec resends what matters; a lost DATA is like one lost on air. */
		counters.tx_q_full++;
		LOG_WRN("tx queue full, dropped kind %d", lsec_frame_kind(buf, len));
		return;
	}
	wake_link();
}

#ifdef LINK_TEST_HOOKS
/* The last sealed DATA frame sent, kept for replay tests. */
static uint8_t last_data[LSEC_MAX_FRAME];
static size_t last_data_len;

int ctrl_link_replay_last(void)
{
	if (last_data_len == 0) {
		return -ENOENT;
	}

	k_mutex_lock(&lock, K_FOREVER);
	enqueue(last_data, last_data_len);
	k_mutex_unlock(&lock);
	return 0;
}
#endif

/* Called from inside lsec, with the lock held. Only queues: see transmit(). */
static void io_send(void *user, const uint8_t *buf, size_t len)
{
#ifdef LINK_TEST_HOOKS
	if (lsec_frame_kind(buf, len) == KIND_DATA) {
		memcpy(last_data, buf, len);
		last_data_len = len;
	}
#endif

	enqueue(buf, len);
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

static bool pairing_now(void)
{
	return ctrl.scanning || lsec_ctrl_pairing_active(&ctrl);
}

/* Scanning and pairing happen on their own short-range radio profile. */
static void update_profile(void)
{
	enum link_profile want = pairing_now() ? LINK_PROFILE_PAIR : LINK_PROFILE_LINK;

	if (tp->profile() != want) {
		tp->set_profile(want);
	}
}

/*
 * How long after a controller frame of @p kind its satellite's answer may
 * still be on the air, from the end of the frame: 0 for frames that draw
 * no answer. The same for our frames and for other networks'.
 */
static uint32_t answer_window_us(int kind)
{
	size_t len;

	switch (kind) {
	case KIND_JOIN_REQ:
		len = LINK_HDR + NONCE_JOIN + TAG_BYTES;   /* JOIN_RSP */
		break;
	case KIND_JOIN_OK:
		len = SEAL_HDR + TAG_BYTES;                /* JOIN_ACK */
		break;
	case KIND_GROUP_UPDATE:
		len = SEAL_HDR + 1 + TAG_BYTES;            /* GROUP_ACK */
		break;
	default:
		return 0;
	}
	return (LINK_TURNAROUND_MS + ANSWER_DEFER_MS + tp->airtime_ms(len) + LINK_SLOT_GUARD_MS) *
	       1000;
}

/* A frame from another network: learn when that network is on the air. */
static void note_foreign(const uint8_t *buf, size_t len, const struct link_rx_info *info)
{
	struct lsec_hdr h;

	if (lsec_parse_hdr(buf, len, &h) || KIND_IS_PAIR(h.kind) ||
	    h.net_tag == (uint16_t)ctrl.net_id) {
		return;
	}

	bool from_ctrl = h.src == LSEC_ADDR_CTRL;

	link_foreign_heard(&foreign, h.net_tag, h.kind == KIND_POLL, from_ctrl,
			   info->time_us - (int64_t)tp->airtime_ms(len) * 1000, info->time_us,
			   from_ctrl ? answer_window_us(h.kind) : 0);
}

/* The air time a queued frame takes, with the answers it reserves. */
static uint32_t need_us(const struct tx_frame *f, int kind)
{
	uint32_t air = tp->airtime_ms(f->len) * 1000;

	if (f->poll) {
		return air + link_replies_end_us(0, f->count, f->slot_ms) +
		       LINK_SLOT_GUARD_MS * 1000;
	}
	return air + answer_window_us(kind);
}

/*
 * Send one queued frame, on the profile its kind belongs to: a PAIR_ABORT
 * queued as pairing ends must still reach a satellite listening at the
 * pairing profile. Returns how long to stay quiet afterwards.
 */
static int64_t transmit(const struct tx_frame *f)
{
	int kind = lsec_frame_kind(f->buf, f->len);
	enum link_profile want = KIND_IS_PAIR(kind) ? LINK_PROFILE_PAIR : LINK_PROFILE_LINK;
	uint32_t answer_ms;
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	if (tp->profile() != want) {
		tp->set_profile(want);
	}
	k_mutex_unlock(&lock);

	/*
	 * CSMA/CA: a random backoff that grows with the channel utilization,
	 * so that controllers whose cycles line up do not also meet in the
	 * same sense. A poll carries SAFE and FIRE: never wait on someone
	 * else for long.
	 */
	link_csma_wait(tp, &link_csma_ctrl, &util, &lock, &counters.csma);

	int64_t start = link_now_us();

	ret = tp->send(f->buf, f->len);
	if (ret) {
		LOG_ERR("send: %d", ret);
	}

	k_mutex_lock(&lock, K_FOREVER);
	link_util_add(&util, start, link_now_us());
	k_mutex_unlock(&lock);

	if (f->poll) {
		int64_t end = link_now_us();

		k_mutex_lock(&lock, K_FOREVER);
		counters.polls++;
		counters.last_poll_end_us = end;
		poll_start_us = start;
		poll_len_us = end - start + link_replies_end_us(0, f->count, f->slot_ms) +
			      LINK_SLOT_GUARD_MS * 1000;
		if (IS_ENABLED(CONFIG_LINK_FOREIGN)) {
			link_foreign_min_cycle(&foreign, poll_len_us);
		}
		polls_done++;
		k_mutex_unlock(&lock);

		/* Every slot, plus the guard for the last reply to arrive. */
		return link_replies_end_us(0, f->count, f->slot_ms) + LINK_SLOT_GUARD_MS * 1000;
	}

	/* Leave room for the satellite's answer before the next frame. */
	answer_ms = tp->airtime_ms(KIND_IS_PAIR(kind) ? PAIR_ANSWER_LEN : LINK_ANSWER_LEN);
	return (int64_t)(LINK_TURNAROUND_MS + ANSWER_DEFER_MS + answer_ms + LINK_SLOT_GUARD_MS) *
	       1000;
}

/* Seal the next poll, which queues it. False if there is none to send. */
static bool seal_next_poll(void)
{
	struct poll_req req;
	bool sealed = false;

	k_mutex_lock(&lock, K_FOREVER);

	while (!sealed && k_msgq_get(&poll_q, &req, K_NO_WAIT) == 0) {
		/* The radio is on the pairing profile: the links wait, as in lsec. */
		if (pairing_now()) {
			polls_done++;
			continue;
		}

		sealing_poll = &req;
		sealed = lsec_ctrl_send_poll(&ctrl, req.mask, req.slot_ms, req.data, req.len) == 0;
		sealing_poll = NULL;
		if (!sealed) {
			polls_done++;
		}
	}

	k_mutex_unlock(&lock);
	return sealed;
}

static void link_main(void *p1, void *p2, void *p3)
{
	static uint8_t buf[LSEC_MAX_FRAME];
	static struct tx_frame f;
	int64_t quiet_until = 0;
	/* f is dequeued and waits for a gap in the other networks' air time. */
	bool held = false;
	bool counted = false;
	int64_t hold_until = 0;
	int64_t hold_deadline = 0;

	while (true) {
		struct link_rx_info info = {0};
		int64_t now = link_now_us();
		int64_t wait_us;
		int len;

		/* Listen through a poll's reply slots; otherwise just briefly. */
		if (now < quiet_until) {
			wait_us = quiet_until - now;
		} else if (held) {
			wait_us = hold_until > now ? hold_until - now : 0;
		} else if (k_msgq_num_used_get(&tx_q) || k_msgq_num_used_get(&poll_q)) {
			wait_us = 0;
		} else {
			wait_us = tp->wake ? RX_WINDOW_US : RX_WINDOW_SHORT_US;
		}

		len = tp->recv(buf, sizeof(buf), K_USEC(wait_us), &info);
		/* Whatever woke it, the queues are looked at below. */
		atomic_clear(&woken);

		k_mutex_lock(&lock, K_FOREVER);
		if (len > 0) {
			last_rssi = info.rssi_dbm;
			last_snr = info.snr_db;
			counters.last_rx_us = info.time_us;
			link_util_add(&util, info.time_us - (int64_t)tp->airtime_ms(len) * 1000,
				      info.time_us);
			if (IS_ENABLED(CONFIG_LINK_FOREIGN)) {
				note_foreign(buf, len, &info);
			}
			lsec_ctrl_rx(&ctrl, buf, len, info.rssi_dbm, k_uptime_get());
		}
		lsec_ctrl_poll(&ctrl, k_uptime_get());
		update_profile();
		k_mutex_unlock(&lock);

		now = link_now_us();
		if (now < quiet_until) {
			continue;
		}

		if (!held && (k_msgq_get(&tx_q, &f, K_NO_WAIT) == 0 ||
			      (seal_next_poll() && k_msgq_get(&tx_q, &f, K_NO_WAIT) == 0))) {
			held = true;
			counted = false;
			hold_deadline = now + LINK_FOREIGN_MAX_WAIT_MS * 1000;
		}
		if (!held) {
			continue;
		}

		/* Pairing runs on its own profile, next to the devices. */
		int kind = lsec_frame_kind(f.buf, f.len);
		int64_t at = KIND_IS_PAIR(kind) || !IS_ENABLED(CONFIG_LINK_FOREIGN_HOLD)
				     ? now
				     : link_foreign_start(&foreign, now, need_us(&f, kind),
							  MAX(hold_deadline - now, 0));

		if (at > now) {
			if (!counted) {
				counted = true;
				counters.held++;
			}
			hold_until = at;
			continue;
		}

		held = false;
		int64_t quiet = transmit(&f);

		quiet_until = link_now_us() + quiet;
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
	uint32_t seed;
	int ret;

	event_cb = cb;
	tp = transport;
	link_foreign_init(&foreign);
	link_util_init(&util, link_now_us());

	/* The network id is needed before the pairings can be loaded. */
	ret = lsec_store_load(&net_id, NULL, NULL);
	if (ret < 0) {
		return ret;
	}

	ret = lsec_ctrl_init(&ctrl, &io, net_id);
	if (ret) {
		return ret;
	}

	if (lsec_random(&seed, sizeof(seed)) == 0) {
		link_sched_seed(seed);
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

int ctrl_link_poll(uint8_t mask, const void *data, size_t len, size_t reply_len)
{
	struct poll_req req = {.mask = mask, .len = len};
	int slot_ms;
	int ret = 0;

	if (mask == 0 || len > sizeof(req.data)) {
		return mask == 0 ? -EINVAL : -EMSGSIZE;
	}
	memcpy(req.data, data, len);

	k_mutex_lock(&lock, K_FOREVER);

	if (pairing_now()) {
		ret = -EAGAIN;
	} else {
		slot_ms = link_slot_ms(tp->airtime_ms(reply_len + LSEC_OVERHEAD));
		if (slot_ms < 0) {
			ret = -EMSGSIZE;
		} else {
			req.slot_ms = slot_ms;
			if (k_msgq_put(&poll_q, &req, K_NO_WAIT)) {
				counters.poll_q_full++;
				ret = -EBUSY;
			} else {
				polls_queued++;
				wake_link();
			}
		}
	}

	k_mutex_unlock(&lock);
	return ret;
}

int64_t ctrl_link_next_poll_ms(uint32_t period_ms)
{
	int64_t at = 0;

	k_mutex_lock(&lock, K_FOREVER);
	if (polls_queued == polls_done && poll_start_us) {
		uint32_t period_us = period_ms * 1000;

		at = poll_start_us + period_us;
		if (IS_ENABLED(CONFIG_LINK_FOREIGN)) {
			at += link_foreign_shift_us(&foreign, link_now_us(), poll_start_us,
						    poll_len_us, period_us);
		}
		at = MAX(at / 1000, 1);
	}
	k_mutex_unlock(&lock);

	return at;
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

uint8_t ctrl_link_joined_mask(void)
{
	uint8_t mask;

	k_mutex_lock(&lock, K_FOREVER);
	mask = lsec_ctrl_joined_mask(&ctrl);
	k_mutex_unlock(&lock);

	return mask;
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
	*out = counters;
	out->net_id = ctrl.net_id;
	out->scanning = ctrl.scanning;
	out->pairing = lsec_ctrl_pairing_active(&ctrl);
	out->rssi_dbm = last_rssi;
	out->snr_db = last_snr;
	out->stats = ctrl.stats;
	out->joined_mask = lsec_ctrl_joined_mask(&ctrl);
	out->util_pct = link_util_pct(&util, link_now_us());
	out->foreign_nets = IS_ENABLED(CONFIG_LINK_FOREIGN)
				    ? link_foreign_count(&foreign, link_now_us())
				    : 0;
	k_mutex_unlock(&lock);
}
