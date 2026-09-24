/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Secure-link mode. One board takes the controller role, the other the
 * satellite role. Each runs the same link code as the real device
 * (../common/link/ctrl_link.c or sat_link.c); this file only adds the
 * bench's OLED, PRG button and a ping test on top:
 *
 *   - Satellite: hold PRG (or "bench pair") to enter pairing mode.
 *   - Controller: hold PRG to pair with the first satellite it hears in
 *     pairing mode, or "bench scan" and "bench pair <uid>".
 *   - Both OLEDs then show the same 4x6 pattern. Press PRG on each to
 *     confirm, hold it to reject.
 *   - Once joined, the controller polls the satellite twice a second, as
 *     the real controller does, which keeps the session up. PRG on the
 *     controller (or "bench ping") runs sealed pings, alternating unicast
 *     and group frames; "bench poll" measures where the satellite's
 *     replies land in their slots.
 *
 * Link events arrive on the link thread. The shell and the button queue
 * requests for secure_main()'s thread, which also drives the display.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "bench.h"
#include "ctrl_link.h"
#include "link_sched.h"
#include "lsec_store.h"
#include "oled.h"
#include "radio.h"
#include "sat_link.h"
#include "secure.h"
#include "trace.h"

LOG_MODULE_REGISTER(secure, LOG_LEVEL_INF);

/*
 * The bench has one satellite. It is paired into the last slot, so that
 * "bench poll" can put phantom satellites ahead of it and test any rank.
 */
#define BENCH_PAIR_SLOT LSEC_MAX_SATS

#define LOOP_PERIOD        K_MSEC(50)
#define DISPLAY_REFRESH_MS 1000
#define LONG_PRESS_MS      1000
/* Controller: poll the satellite this often when nothing else is running. */
#define HEARTBEAT_MS       500
#define JAM_LEN            255

static enum bench_role role;
/* Written by both threads; st_lock keeps each update whole. */
static struct secure_status st;
static K_MUTEX_DEFINE(st_lock);
static atomic_t st_changed;

static struct {
	uint32_t count;
	uint32_t interval_ms;
	atomic_t seq;
	int64_t next_at;
	int64_t sent_at;
} ping;

/* A "bench poll" run, and where its replies landed. */
static struct {
	uint32_t count;
	uint32_t interval_ms;
	uint8_t mask;
	uint8_t reply_len;
	uint8_t rank;
	uint8_t slot_ms;
	atomic_t seq;
	int64_t next_at;
	uint32_t answered;
	uint32_t late;           /* ended after their slot */
	int64_t min_us, max_us, sum_us;
} poll;

static int64_t next_heartbeat;

/* The paired satellite's slot, found by looking; 0 when there is none. */
static atomic_t sat_slot;

struct secure_req {
	enum secure_req_type type;
	uint64_t uid;
	uint32_t arg;
	uint32_t arg2;
};

K_MSGQ_DEFINE(requests, sizeof(struct secure_req), 8, 4);

/* Controller: pair with the first satellite heard (long press). */
static atomic_t auto_pair;

/* ---- role in settings ------------------------------------------------- */

static int role_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
		    void *param)
{
	uint8_t v;

	if (settings_name_steq(key, "role", NULL) && len == 1 &&
	    read_cb(cb_arg, &v, 1) == 1 && v <= ROLE_SAT) {
		*(enum bench_role *)param = v;
	}

	return 0;
}

enum bench_role secure_role_load(void)
{
	enum bench_role r = ROLE_OFF;

	settings_load_subtree_direct("bench", role_set, &r);
	return r;
}

int secure_role_save(enum bench_role r)
{
	uint8_t v = r;
	int ret;

	/* Pairings belong to the role that made them. */
	ret = lsec_store_clear();
	if (ret) {
		return ret;
	}

	return settings_save_one("bench/role", &v, 1);
}

/* ---- requests --------------------------------------------------------- */

int secure_request(enum secure_req_type type, uint64_t uid, uint32_t arg, uint32_t arg2)
{
	struct secure_req req = {.type = type, .uid = uid, .arg = arg, .arg2 = arg2};

	if (role == ROLE_OFF) {
		return -ENOTSUP;
	}

	return k_msgq_put(&requests, &req, K_NO_WAIT) ? -EBUSY : 0;
}

const struct secure_status *secure_status(void)
{
	return &st;
}

void secure_button(bool pressed)
{
	static int64_t down_at;
	bool long_press;

	if (pressed) {
		down_at = k_uptime_get();
		return;
	}

	long_press = k_uptime_get() - down_at >= LONG_PRESS_MS;

	if (st.sas_shown) {
		secure_request(long_press ? REQ_REJECT : REQ_CONFIRM, 0, 0, 0);
	} else if (long_press) {
		secure_request(REQ_PAIR, 0, 0, 0);
	} else if (role == ROLE_CTRL) {
		secure_request(st.pinging ? REQ_STOP : REQ_PING, 0, 10, 2000);
	}
}

static void note(const char *text)
{
	k_mutex_lock(&st_lock, K_FOREVER);
	st.note = text;
	k_mutex_unlock(&st_lock);
	atomic_set(&st_changed, 1);
}

/* ---- link events (link thread) ---------------------------------------- */

static const char *fail_name(enum lsec_fail reason)
{
	switch (reason) {
	case LSEC_FAIL_TIMEOUT:
		return "timeout";
	case LSEC_FAIL_REJECTED:
		return "rejected here";
	case LSEC_FAIL_PEER_ABORT:
		return "peer aborted";
	case LSEC_FAIL_COMMITMENT:
		return "bad commitment";
	case LSEC_FAIL_CHECK:
		return "key check failed";
	case LSEC_FAIL_STORE:
		return "store failed";
	default:
		return "crypto error";
	}
}

static void log_sas(const uint8_t sas[LSEC_SAS_FRAMES])
{
	char rows[LSEC_SAS_FRAMES][LSEC_SAS_LEDS + 1];

	for (int r = 0; r < LSEC_SAS_FRAMES; r++) {
		for (int led = 0; led < LSEC_SAS_LEDS; led++) {
			rows[r][led] = (sas[r] & BIT(led)) ? '#' : '.';
		}
		rows[r][LSEC_SAS_LEDS] = '\0';
	}

	LOG_INF("compare: %s %s %s %s  (PRG = match, hold PRG = no match)", rows[0], rows[1],
		rows[2], rows[3]);
}

static void on_data(const struct lsec_event *ev)
{
	struct bench_pkt pkt;

	/* A pong may be padded to test longer replies. */
	if (ev->len < sizeof(pkt)) {
		LOG_INF("%zu-byte payload from slot %u", ev->len, ev->slot);
		return;
	}

	memcpy(&pkt, ev->data, sizeof(pkt));
	if (pkt.magic != BENCH_MAGIC) {
		return;
	}

	if (role == ROLE_SAT && pkt.type == BENCH_PING) {
		struct sat_link_state ls;

		sat_link_state(&ls);

		struct bench_pkt pong = {
			.magic = BENCH_MAGIC,
			.type = BENCH_PONG,
			.src = bench_node_id(),
			.dst = pkt.src,
			.seq = pkt.seq,
			.rssi_dbm = ls.rssi_dbm,
			.snr_db = ls.snr_db,
		};

		LOG_INF("sealed ping %u (%s): %d dBm", pkt.seq, ev->group ? "group" : "unicast",
			ls.rssi_dbm);
		sat_link_send(&pong, sizeof(pong));
		return;
	}

	if (role == ROLE_CTRL && pkt.type == BENCH_PONG && st.pinging && poll.count &&
	    pkt.seq == atomic_get(&poll.seq)) {
		struct ctrl_link_state ls;
		int64_t off;
		int64_t slot_end;

		/* The reception time of this very frame, against the poll's end. */
		ctrl_link_state(&ls);
		off = ls.last_rx_us - ls.last_poll_end_us;
		slot_end = link_slot_start_us(0, poll.rank + 1, poll.slot_ms);

		poll.answered++;
		poll.min_us = MIN(poll.min_us, off);
		poll.max_us = MAX(poll.max_us, off);
		poll.sum_us += off;
		if (off > slot_end) {
			poll.late++;
		}

		k_mutex_lock(&st_lock, K_FOREVER);
		st.pongs++;
		st.rtt_ms = off / 1000;
		k_mutex_unlock(&st_lock);

		LOG_INF("poll %u: reply at +%lld.%01lld ms, slot %lld-%lld ms, there %d dBm",
			pkt.seq, off / 1000, (off % 1000) / 100,
			link_slot_start_us(0, poll.rank, poll.slot_ms) / 1000, slot_end / 1000,
			pkt.rssi_dbm);
		return;
	}

	/* Seq 0 answers a heartbeat poll. */
	if (role == ROLE_CTRL && pkt.type == BENCH_PONG && pkt.seq != 0 &&
	    pkt.seq == atomic_get(&ping.seq)) {
		struct ctrl_link_state ls;
		uint32_t rtt = k_uptime_get() - ping.sent_at;
		bool group = pkt.seq % 2 == 0;

		ctrl_link_state(&ls);

		k_mutex_lock(&st_lock, K_FOREVER);
		st.rtt_ms = rtt;
		st.pongs++;
		st.group_pongs += group;
		k_mutex_unlock(&st_lock);

		LOG_INF("sealed ping %u (%s): pong in %u ms, there %d dBm, back %d dBm", pkt.seq,
			group ? "group" : "unicast", rtt, pkt.rssi_dbm, ls.rssi_dbm);
	}
}

/* Satellite: answer a poll with a pong, padded to what the poll asks for. */
static void on_poll(const struct lsec_event *ev)
{
	struct sat_link_state ls;
	struct bench_pkt pkt = {0};
	uint8_t reply[LSEC_MAX_PAYLOAD] = {0};
	size_t len = sizeof(pkt);
	int ret;

	if (ev->len == sizeof(pkt)) {
		memcpy(&pkt, ev->data, sizeof(pkt));
	}
	if (pkt.magic == BENCH_MAGIC && pkt.type == BENCH_POLL) {
		len = CLAMP(pkt.dst, sizeof(pkt), sizeof(reply));
	}

	sat_link_state(&ls);

	struct bench_pkt pong = {
		.magic = BENCH_MAGIC,
		.type = BENCH_PONG,
		.src = bench_node_id(),
		.seq = pkt.seq,
		.rssi_dbm = ls.rssi_dbm,
		.snr_db = ls.snr_db,
	};

	memcpy(reply, &pong, sizeof(pong));
	ret = sat_link_reply(reply, len);

	/* Seq 0 is the controller's heartbeat, twice a second. */
	if (pkt.seq != 0) {
		LOG_INF("poll %u: slot %u of %u, %u ms each, %d dBm%s", pkt.seq, ev->rank,
			ev->count, ev->slot_ms, ls.rssi_dbm, ret ? ", reply too long" : "");
	}
}

static void on_event(const struct lsec_event *ev)
{
	switch (ev->type) {
	case LSEC_EV_ADV: {
		atomic_val_t ap = atomic_get(&auto_pair);

		LOG_INF("satellite %016llx in pairing mode, %d dBm", ev->uid, ev->rssi_dbm);
		if (ap && atomic_cas(&auto_pair, ap, 0)) {
			secure_request(REQ_PAIR, ev->uid, ap >> 8, 0);
		}
		break;
	}
	case LSEC_EV_SAS:
		k_mutex_lock(&st_lock, K_FOREVER);
		memcpy(st.sas, ev->sas, sizeof(st.sas));
		st.sas_shown = true;
		st.note = "compare pattern";
		k_mutex_unlock(&st_lock);
		log_sas(ev->sas);
		break;
	case LSEC_EV_PAIRED:
		k_mutex_lock(&st_lock, K_FOREVER);
		st.sas_shown = false;
		st.note = "paired";
		k_mutex_unlock(&st_lock);
		LOG_INF("paired: satellite %016llx in slot %u", ev->uid, ev->slot);
		break;
	case LSEC_EV_PAIR_FAILED:
		k_mutex_lock(&st_lock, K_FOREVER);
		st.sas_shown = false;
		st.note = fail_name(ev->reason);
		k_mutex_unlock(&st_lock);
		LOG_WRN("pairing failed: %s", fail_name(ev->reason));
		break;
	case LSEC_EV_JOINED:
		note("joined");
		LOG_INF("session up, slot %u", ev->slot);
		break;
	case LSEC_EV_DATA:
		on_data(ev);
		break;
	case LSEC_EV_POLL:
		on_poll(ev);
		break;
	case LSEC_EV_LEFT:
		note("session dropped");
		LOG_WRN("session dropped: %s for %u ms",
			role == ROLE_CTRL ? "no reply" : "not polled",
			role == ROLE_CTRL ? LSEC_PEER_TIMEOUT_MS : LSEC_SESSION_TIMEOUT_MS);
		break;
	}

	atomic_set(&st_changed, 1);
}

/* ---- the loop --------------------------------------------------------- */

static void handle_request(const struct secure_req *req, int64_t now)
{
	int ret = 0;

	switch (req->type) {
	case REQ_CONFIRM:
	case REQ_REJECT:
		if (role == ROLE_CTRL) {
			ctrl_link_pair_confirm(req->type == REQ_CONFIRM);
		} else {
			sat_link_pair_confirm(req->type == REQ_CONFIRM);
		}
		if (req->type == REQ_CONFIRM && st.sas_shown) {
			note("confirmed, waiting");
		}
		break;

	case REQ_PAIR:
		if (role == ROLE_SAT) {
			LOG_INF("pairing mode for %d s", LSEC_PAIR_WINDOW_MS / 1000);
			ret = sat_link_pair_start();
			note("pairing mode");
		} else if (req->uid == 0) {
			LOG_INF("pairing with the first satellite in pairing mode");
			/* The slot goes with it, in the upper bits. */
			atomic_set(&auto_pair, 1 | (req->arg << 8));
			ctrl_link_scan(true);
			note("looking");
		} else {
			ret = ctrl_link_pair_start(req->uid, req->arg ? req->arg : BENCH_PAIR_SLOT);
			note("pairing");
		}
		break;

	case REQ_SCAN:
		if (role == ROLE_CTRL) {
			atomic_clear(&auto_pair);
			ctrl_link_scan(req->arg);
		}
		break;

	case REQ_UNPAIR:
		ret = role == ROLE_CTRL ? ctrl_link_forget(atomic_get(&sat_slot)) : sat_link_unpair();
		note("unpaired");
		break;

	case REQ_REPLAY:
		ret = role == ROLE_CTRL ? ctrl_link_replay_last() : sat_link_replay_last();
		LOG_INF("replayed the last sealed DATA frame: %d", ret);
		break;

	case REQ_PING:
		if (role == ROLE_CTRL) {
			/* A ping sent while the last PONG is still on air would collide with it. */
			uint32_t airtime = radio_airtime_ms(sizeof(struct bench_pkt) + LSEC_OVERHEAD);
			uint32_t min_interval = 2 * airtime;

			ping.count = req->arg;
			ping.interval_ms = MAX(req->arg2, min_interval);
			atomic_set(&ping.seq, 0);
			ping.next_at = now;

			k_mutex_lock(&st_lock, K_FOREVER);
			st.pinging = true;
			st.ping_total = ping.count;
			st.ping_sent = 0;
			st.pongs = 0;
			st.group_pongs = 0;
			k_mutex_unlock(&st_lock);

			LOG_INF("sealed pings: %u every %u ms, odd unicast, even group", ping.count,
				ping.interval_ms);
		}
		break;

	case REQ_STOP:
		ping.count = 0;
		poll.count = 0;
		break;

	case REQ_POLL: {
		uint8_t slot = atomic_get(&sat_slot);
		int air;

		if (role != ROLE_CTRL || slot == 0) {
			ret = -ENOTCONN;
			break;
		}

		poll.count = req->arg;
		poll.interval_ms = req->arg2;
		poll.reply_len = CLAMP((req->uid >> 8) & 0xFF, sizeof(struct bench_pkt),
				       LSEC_MAX_PAYLOAD);
		/* Phantoms: satellites that are not there, ahead of ours. */
		poll.mask = (req->uid & 0xFF) | BIT(slot - 1);
		poll.rank = __builtin_popcount(poll.mask & (BIT(slot - 1) - 1));
		air = radio_airtime_ms(poll.reply_len + LSEC_OVERHEAD);
		poll.slot_ms = link_slot_ms(air);
		atomic_set(&poll.seq, 0);
		poll.next_at = now;
		poll.answered = 0;
		poll.late = 0;
		poll.min_us = INT64_MAX;
		poll.max_us = 0;
		poll.sum_us = 0;

		k_mutex_lock(&st_lock, K_FOREVER);
		st.pinging = true;
		st.ping_total = poll.count;
		st.ping_sent = 0;
		st.pongs = 0;
		st.group_pongs = 0;
		k_mutex_unlock(&st_lock);

		LOG_INF("polls: %u every %u ms, mask 0x%02x: our satellite has slot %u of %u, "
			"%u ms each (%u-byte reply, %d ms on air)",
			poll.count, poll.interval_ms, poll.mask, poll.rank,
			__builtin_popcount(poll.mask), poll.slot_ms, poll.reply_len, air);
		break;
	}

	case REQ_JAM: {
		static const uint8_t jam[JAM_LEN];
		int64_t end = now + req->arg;
		int frames = 0;

		/* Straight to the radio, past the link and its listen-before-talk. */
		while (k_uptime_get() < end && radio_send(jam, sizeof(jam)) == 0) {
			frames++;
		}
		LOG_INF("jammed for %u ms: %d frames", req->arg, frames);
		break;
	}
	}

	if (ret) {
		LOG_WRN("request %d: %d", req->type, ret);
	}
	atomic_set(&st_changed, 1);
}

static void run_ping(int64_t now)
{
	uint32_t seq = atomic_get(&ping.seq);

	if (!st.pinging || now < ping.next_at) {
		return;
	}

	if (seq >= ping.count) {
		st.pinging = false;
		atomic_set(&st_changed, 1);
		LOG_INF("%u of %u answered (%u of them group)", st.pongs, seq, st.group_pongs);
		return;
	}

	seq++;

	struct bench_pkt pkt = {
		.magic = BENCH_MAGIC,
		.type = BENCH_PING,
		.src = bench_node_id(),
		.seq = seq,
	};
	uint8_t dst = seq % 2 == 0 ? LSEC_ADDR_GROUP : atomic_get(&sat_slot);
	int ret;

	ping.sent_at = now;
	ping.next_at = now + ping.interval_ms;
	atomic_set(&ping.seq, seq);
	st.ping_sent = seq;
	atomic_set(&st_changed, 1);

	ret = ctrl_link_send(dst, &pkt, sizeof(pkt));
	if (ret) {
		LOG_WRN("ping %u: not sent (%d), no session", seq, ret);
	}
}

static void send_poll(uint8_t mask, uint32_t seq, uint8_t reply_len)
{
	struct bench_pkt pkt = {
		.magic = BENCH_MAGIC,
		.type = BENCH_POLL,
		.src = bench_node_id(),
		.dst = reply_len,
		.seq = seq,
	};
	int ret = ctrl_link_poll(mask, &pkt, sizeof(pkt), reply_len);

	if (ret && seq != 0) {
		LOG_WRN("poll %u: not sent (%d)", seq, ret);
	}
}

static void run_poll(int64_t now)
{
	uint32_t seq = atomic_get(&poll.seq);

	if (now < poll.next_at) {
		return;
	}

	if (seq >= poll.count) {
		st.pinging = false;
		poll.count = 0;
		atomic_set(&st_changed, 1);
		if (poll.answered) {
			int64_t mean = poll.sum_us / poll.answered;

			LOG_INF("%u of %u answered, at +%lld.%01lld to +%lld.%01lld ms (mean "
				"%lld.%01lld), %u after their slot",
				poll.answered, seq, poll.min_us / 1000, (poll.min_us % 1000) / 100,
				poll.max_us / 1000, (poll.max_us % 1000) / 100, mean / 1000,
				(mean % 1000) / 100, poll.late);
		} else {
			LOG_INF("0 of %u answered", seq);
		}
		return;
	}

	seq++;
	atomic_set(&poll.seq, seq);
	poll.next_at = now + poll.interval_ms;
	st.ping_sent = seq;
	atomic_set(&st_changed, 1);

	send_poll(poll.mask, seq, poll.reply_len);
}

/* Keep the session up as the real controller does, unless a test runs. */
static void run_heartbeat(int64_t now)
{
	uint8_t slot = atomic_get(&sat_slot);

	if (st.pinging || slot == 0 || !ctrl_link_joined(slot) || now < next_heartbeat) {
		return;
	}

	next_heartbeat = now + HEARTBEAT_MS;
	send_poll(BIT(slot - 1), 0, sizeof(struct bench_pkt));
}

/* The controller's satellite is in whichever slot it was paired into. */
static uint8_t find_slot(void)
{
	for (uint8_t slot = LSEC_MAX_SATS; slot >= 1; slot--) {
		if (ctrl_link_paired_uid(slot)) {
			return slot;
		}
	}
	return 0;
}

/*
 * Copy the link's own state into the display status. The link lock is
 * taken before st_lock everywhere (event handlers run under it), so the
 * link is read first.
 */
static void refresh_link_state(void)
{
	struct secure_status now = {0};

	if (role == ROLE_CTRL) {
		struct ctrl_link_state ls;

		ctrl_link_state(&ls);
		now.slot = find_slot();
		atomic_set(&sat_slot, now.slot);
		now.uid = now.slot ? ctrl_link_paired_uid(now.slot) : 0;
		now.paired = now.uid != 0;
		now.joined = now.slot && ctrl_link_joined(now.slot);
		now.scanning = ls.scanning;
		now.pairing = ls.scanning || ls.pairing;
		now.stats = ls.stats;
	} else {
		struct sat_link_state ls;

		sat_link_state(&ls);
		now.slot = ls.slot;
		now.uid = ls.uid;
		now.paired = ls.paired;
		now.joined = ls.joined;
		now.pairing = ls.pairing;
		now.stats = ls.stats;
	}

	k_mutex_lock(&st_lock, K_FOREVER);
	st.uid = now.uid;
	st.paired = now.paired;
	st.joined = now.joined;
	st.scanning = now.scanning;
	st.pairing = now.pairing;
	st.stats = now.stats;
	st.slot = now.slot;
	k_mutex_unlock(&st_lock);
}

void secure_main(enum bench_role r)
{
	int64_t last_draw = 0;
	int ret;

	role = r;
	st.role = r;

	/* The radio, through the frame trace of "bench trace". */
	ret = role == ROLE_CTRL ? ctrl_link_init(on_event, &trace_transport)
				: sat_link_init(on_event, &trace_transport);
	if (ret) {
		LOG_ERR("link init: %d", ret);
		note("init failed");
	}

	refresh_link_state();
	if (ret == 0 && !st.paired) {
		LOG_INF("not paired; hold PRG on both boards to pair");
		note("not paired");
	}

	while (true) {
		struct secure_req req;
		int64_t now = k_uptime_get();

		while (k_msgq_get(&requests, &req, K_NO_WAIT) == 0) {
			handle_request(&req, now);
		}

		if (role == ROLE_CTRL) {
			if (poll.count) {
				run_poll(now);
			} else {
				run_ping(now);
			}
			run_heartbeat(now);
		}

		refresh_link_state();

		if (atomic_cas(&st_changed, 1, 0) || now - last_draw >= DISPLAY_REFRESH_MS) {
			k_mutex_lock(&st_lock, K_FOREVER);
			oled_show_secure(bench_node_id(), &st);
			k_mutex_unlock(&st_lock);
			last_draw = now;
		}

		k_sleep(LOOP_PERIOD);
	}
}
