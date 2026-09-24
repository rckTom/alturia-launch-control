/* SPDX-License-Identifier: Apache-2.0 */

/*
 * lsec unit tests. Every node sits on one simulated "air": each sent frame
 * reaches every other running node unless the test's filter drops, alters
 * or reroutes it. Time is simulated too.
 */

#include <string.h>
#include <zephyr/ztest.h>

#include "lsec.h"
#include "lsec_internal.h"

enum {
	N_CTRL,
	N_SAT1,
	N_SAT2,
	N_MITM_SAT,   /* attacker posing as SAT1 towards the controller */
	N_MITM_CTRL,  /* attacker posing as a controller towards SAT1 */
	N_NODES,
};

#define UID1 0x1111222233334444ULL
#define UID2 0x5555666677778888ULL
#define NET  0xC0FFEE01u

struct node {
	bool is_ctrl;
	bool up;
	struct lsec_ctrl *ctrl;
	struct lsec_sat *sat;

	uint8_t sas[LSEC_SAS_FRAMES];
	int sas_count;
	int paired;
	int joined;
	int fails;
	enum lsec_fail fail;
	int advs;
	uint64_t adv_uid;
	int datas;
	uint8_t data[LSEC_MAX_PAYLOAD];
	size_t data_len;
	bool data_group;
	uint8_t data_slot;
	int polls;
	struct lsec_event poll;          /* the last POLL, data copied to poll_data */
	uint8_t poll_data[LSEC_MAX_PAYLOAD];
	bool auto_reply;                 /* satellite: answer every POLL with empty DATA */
	int lefts;
	struct lsec_pairing stored[LSEC_MAX_SATS + 1];
	bool has_stored[LSEC_MAX_SATS + 1];
};

static struct lsec_ctrl ctrl, mitm_ctrl;
static struct lsec_sat sat1, sat2, mitm_sat;
static struct node nodes[N_NODES];

struct frame {
	int from;
	uint8_t buf[LSEC_MAX_FRAME];
	size_t len;
};

static struct frame air[128];
static int air_head, air_tail;
static int64_t now;

/* Return false to drop. May rewrite the frame. */
static bool (*filter)(int from, int to, uint8_t *buf, size_t *len);

/* The last frame of a kind each node sent, for replay tests. */
static struct frame captured[N_NODES][64];

static void io_send(void *user, const uint8_t *buf, size_t len)
{
	struct node *n = user;
	int from = n - nodes;
	struct frame *f = &air[air_tail++ % ARRAY_SIZE(air)];
	int kind = lsec_frame_kind(buf, len);

	zassert_true(len <= LSEC_MAX_FRAME);
	zassert_true(air_tail - air_head <= (int)ARRAY_SIZE(air), "air overflow");

	f->from = from;
	f->len = len;
	memcpy(f->buf, buf, len);

	if (kind >= 0) {
		captured[from][kind] = *f;
	}
}

static void io_event(void *user, const struct lsec_event *ev)
{
	struct node *n = user;

	switch (ev->type) {
	case LSEC_EV_ADV:
		n->advs++;
		n->adv_uid = ev->uid;
		break;
	case LSEC_EV_SAS:
		n->sas_count++;
		memcpy(n->sas, ev->sas, sizeof(n->sas));
		break;
	case LSEC_EV_PAIRED:
		n->paired++;
		break;
	case LSEC_EV_PAIR_FAILED:
		n->fails++;
		n->fail = ev->reason;
		break;
	case LSEC_EV_JOINED:
		n->joined++;
		break;
	case LSEC_EV_DATA:
		n->datas++;
		n->data_len = ev->len;
		n->data_group = ev->group;
		n->data_slot = ev->slot;
		memcpy(n->data, ev->data, ev->len);
		break;
	case LSEC_EV_POLL:
		n->polls++;
		n->poll = *ev;
		memcpy(n->poll_data, ev->data, ev->len);
		n->poll.data = n->poll_data;
		if (n->auto_reply) {
			zassert_ok(lsec_sat_send(n->sat, NULL, 0));
		}
		break;
	case LSEC_EV_LEFT:
		n->lefts++;
		break;
	}
}

static int io_store(void *user, uint8_t slot, const struct lsec_pairing *p)
{
	struct node *n = user;

	n->has_stored[slot] = p != NULL;
	if (p) {
		n->stored[slot] = *p;
	}

	return 0;
}

static void node_init(int i, uint64_t uid)
{
	struct node *n = &nodes[i];
	struct lsec_io io = {
		.send = io_send,
		.event = io_event,
		.store = io_store,
		.user = n,
	};
	struct lsec_ctrl *c = n->ctrl;
	struct lsec_sat *s = n->sat;
	bool is_ctrl = n->is_ctrl;

	memset(n, 0, sizeof(*n));
	n->ctrl = c;
	n->sat = s;
	n->is_ctrl = is_ctrl;
	n->up = true;

	/*
	 * The session timeouts are off unless a test turns them on: most
	 * tests leave the link idle for longer than they last.
	 */
	if (is_ctrl) {
		zassert_ok(lsec_ctrl_init(c, &io, uid == 0 ? NET : (uint32_t)uid));
		c->peer_timeout_ms = 0;
	} else {
		zassert_ok(lsec_sat_init(s, &io, uid));
		s->session_timeout_ms = 0;
	}
}

static void deliver(int to, const struct frame *f)
{
	uint8_t buf[LSEC_MAX_FRAME];
	size_t len = f->len;

	memcpy(buf, f->buf, len);
	if (filter && !filter(f->from, to, buf, &len)) {
		return;
	}

	if (nodes[to].is_ctrl) {
		lsec_ctrl_rx(nodes[to].ctrl, buf, len, -50, now);
	} else {
		lsec_sat_rx(nodes[to].sat, buf, len, now);
	}
}

static void pump(void)
{
	int guard = 0;

	while (air_head != air_tail) {
		struct frame f = air[air_head++ % ARRAY_SIZE(air)];

		zassert_true(++guard < 1000, "frame storm");

		for (int to = 0; to < N_NODES; to++) {
			if (to != f.from && nodes[to].up) {
				deliver(to, &f);
			}
		}
	}
}

/* Inject a frame as if it came from nowhere in particular. */
static void inject(int to, const struct frame *f)
{
	struct frame g = *f;

	g.from = -1;
	if (nodes[to].is_ctrl) {
		lsec_ctrl_rx(nodes[to].ctrl, g.buf, g.len, -50, now);
	} else {
		lsec_sat_rx(nodes[to].sat, g.buf, g.len, now);
	}
	pump();
}

static void tick(int64_t ms)
{
	for (int64_t t = 0; t < ms; t += 100) {
		now += 100;
		for (int i = 0; i < N_NODES; i++) {
			if (!nodes[i].up) {
				continue;
			}
			if (nodes[i].is_ctrl) {
				lsec_ctrl_poll(nodes[i].ctrl, now);
			} else {
				lsec_sat_poll(nodes[i].sat, now);
			}
		}
		pump();
	}
}

static void setup(void *fixture)
{
	ARG_UNUSED(fixture);

	nodes[N_CTRL].ctrl = &ctrl;
	nodes[N_CTRL].is_ctrl = true;
	nodes[N_SAT1].sat = &sat1;
	nodes[N_SAT2].sat = &sat2;
	nodes[N_MITM_SAT].sat = &mitm_sat;
	nodes[N_MITM_CTRL].ctrl = &mitm_ctrl;
	nodes[N_MITM_CTRL].is_ctrl = true;

	air_head = air_tail = 0;
	now = 1000;
	filter = NULL;
	memset(captured, 0, sizeof(captured));

	node_init(N_CTRL, 0);
	node_init(N_SAT1, UID1);
	node_init(N_SAT2, UID2);
	node_init(N_MITM_SAT, UID1);
	node_init(N_MITM_CTRL, 0xBAD0BAD0);

	/* Attackers only take part in the tests that switch them on. */
	nodes[N_MITM_SAT].up = false;
	nodes[N_MITM_CTRL].up = false;
}

/* Run a pairing between the controller and a satellite up to the SAS. */
static void pair_to_sas(int sat, uint8_t slot)
{
	uint64_t uid = nodes[sat].sat->uid;

	lsec_ctrl_scan(&ctrl, true);
	zassert_ok(lsec_sat_pair_start(nodes[sat].sat, now));
	pump();
	zassert_true(nodes[N_CTRL].advs > 0, "no PAIR_ADV seen");
	zassert_equal(nodes[N_CTRL].adv_uid, uid);
	lsec_ctrl_scan(&ctrl, false);

	zassert_ok(lsec_ctrl_pair_start(&ctrl, uid, slot, now));
	pump();
}

static void pair(int sat, uint8_t slot)
{
	pair_to_sas(sat, slot);
	zassert_equal(nodes[N_CTRL].sas_count, 1);
	zassert_equal(nodes[sat].sas_count, 1);
	zassert_mem_equal(nodes[N_CTRL].sas, nodes[sat].sas, LSEC_SAS_FRAMES);

	lsec_ctrl_pair_confirm(&ctrl, true, now);
	pump();
	lsec_sat_pair_confirm(nodes[sat].sat, true, now);
	pump();

	zassert_equal(nodes[N_CTRL].paired, 1);
	zassert_equal(nodes[sat].paired, 1);
	nodes[N_CTRL].paired = 0;
	nodes[N_CTRL].sas_count = 0;
	/* The satellite lingers for late CHK_C, then leaves pairing mode. */
	tick(LSEC_PAIR_LINGER_MS + 100);
	zassert_false(lsec_sat_pairing_active(nodes[sat].sat));
}

static void join(int sat, uint8_t slot)
{
	for (int i = 0; i < 100 && !(lsec_ctrl_joined(&ctrl, slot) &&
				     lsec_sat_joined(nodes[sat].sat)); i++) {
		tick(100);
	}

	zassert_true(lsec_ctrl_joined(&ctrl, slot), "controller not joined");
	zassert_true(lsec_sat_joined(nodes[sat].sat), "satellite not joined");
}

ZTEST(lsec, test_pair_join_and_talk)
{
	static const char down[] = "ARM 0x3";
	static const char up[] = "status";

	pair(N_SAT1, 1);

	/* Both sides stored the same key for the same satellite. */
	zassert_true(nodes[N_CTRL].has_stored[1]);
	zassert_true(nodes[N_SAT1].has_stored[0]);
	zassert_equal(nodes[N_CTRL].stored[1].sat_uid, UID1);
	zassert_equal(nodes[N_SAT1].stored[0].slot, 1);
	zassert_equal(nodes[N_SAT1].stored[0].net_id, NET);
	zassert_mem_equal(nodes[N_CTRL].stored[1].ltk, nodes[N_SAT1].stored[0].ltk,
			  LSEC_KEY_BYTES);

	join(N_SAT1, 1);

	zassert_ok(lsec_ctrl_send(&ctrl, 1, down, sizeof(down)));
	pump();
	zassert_equal(nodes[N_SAT1].datas, 1);
	zassert_false(nodes[N_SAT1].data_group);
	zassert_mem_equal(nodes[N_SAT1].data, down, sizeof(down));

	zassert_ok(lsec_sat_send(&sat1, up, sizeof(up)));
	pump();
	zassert_equal(nodes[N_CTRL].datas, 1);
	zassert_equal(nodes[N_CTRL].data_slot, 1);
	zassert_mem_equal(nodes[N_CTRL].data, up, sizeof(up));

	/* The other satellite hears the air but is not paired. */
	zassert_equal(nodes[N_SAT2].datas, 0);

	/* The ciphertext does not contain the plaintext. */
	const struct frame *f = &captured[N_CTRL][KIND_DATA];

	zassert_equal(f->len, sizeof(down) + LSEC_OVERHEAD);
	for (size_t i = 0; i + sizeof(down) - 1 <= f->len; i++) {
		zassert_true(memcmp(f->buf + i, down, sizeof(down) - 1) != 0, "plaintext on air");
	}
}

/* Lose the first copy of every pairing frame in both directions. */
static int seen[64];
static int dropped;

static bool drop_first(int from, int to, uint8_t *buf, size_t *len)
{
	int kind = lsec_frame_kind(buf, *len);

	if (KIND_IS_PAIR(kind) && kind != KIND_PAIR_ADV && seen[kind]++ == 0) {
		dropped++;
		return false;
	}
	return true;
}

ZTEST(lsec, test_pairing_survives_loss)
{
	memset(seen, 0, sizeof(seen));
	dropped = 0;

	lsec_ctrl_scan(&ctrl, true);
	zassert_ok(lsec_sat_pair_start(&sat1, now));
	pump();
	zassert_ok(lsec_ctrl_pair_start(&ctrl, UID1, 1, now));
	filter = drop_first;
	pump();

	for (int i = 0; i < 50 && nodes[N_CTRL].sas_count == 0; i++) {
		tick(100);
	}
	zassert_equal(nodes[N_CTRL].sas_count, 1);
	zassert_mem_equal(nodes[N_CTRL].sas, nodes[N_SAT1].sas, LSEC_SAS_FRAMES);

	/* The controller's user confirms first; the satellite's user later. */
	lsec_ctrl_pair_confirm(&ctrl, true, now);
	pump();
	tick(5000);
	zassert_equal(nodes[N_CTRL].paired, 0, "paired before the satellite confirmed");
	lsec_sat_pair_confirm(&sat1, true, now);
	pump();
	for (int i = 0; i < 50 && nodes[N_CTRL].paired == 0; i++) {
		tick(100);
	}

	zassert_equal(nodes[N_CTRL].paired, 1);
	zassert_equal(nodes[N_SAT1].paired, 1);
	zassert_true(dropped >= 4);
	filter = NULL;
	join(N_SAT1, 1);
}

/* A full man in the middle runs its own pairing with each side. */
static bool mitm_route(int from, int to, uint8_t *buf, size_t *len)
{
	int kind = lsec_frame_kind(buf, *len);

	if (!KIND_IS_PAIR(kind)) {
		return true;
	}
	/* Controller <-> fake satellite, fake controller <-> satellite. */
	if (from == N_CTRL) {
		return to == N_MITM_SAT;
	}
	if (from == N_MITM_SAT) {
		return to == N_CTRL;
	}
	if (from == N_MITM_CTRL) {
		return to == N_SAT1;
	}
	if (from == N_SAT1) {
		return to == N_MITM_CTRL;
	}
	return false;
}

ZTEST(lsec, test_mitm_shows_different_sas)
{
	nodes[N_MITM_SAT].up = true;
	nodes[N_MITM_CTRL].up = true;
	filter = mitm_route;

	zassert_ok(lsec_sat_pair_start(&sat1, now));
	zassert_ok(lsec_sat_pair_start(&mitm_sat, now));
	pump();
	zassert_ok(lsec_ctrl_pair_start(&mitm_ctrl, UID1, 1, now));
	zassert_ok(lsec_ctrl_pair_start(&ctrl, UID1, 1, now));
	pump();

	zassert_equal(nodes[N_CTRL].sas_count, 1);
	zassert_equal(nodes[N_SAT1].sas_count, 1);

	/*
	 * Each leg is a valid pairing, so only the user can tell: the two
	 * screens differ except with probability 2^-24.
	 */
	zassert_true(memcmp(nodes[N_CTRL].sas, nodes[N_SAT1].sas, LSEC_SAS_FRAMES) != 0,
		     "MITM produced matching SAS");

	/* The user notices and rejects on the controller. */
	lsec_ctrl_pair_confirm(&ctrl, false, now);
	pump();
	zassert_equal(nodes[N_CTRL].fail, LSEC_FAIL_REJECTED);
	zassert_false(nodes[N_CTRL].has_stored[1]);
}

/* A cruder attacker swaps the public keys in flight without its own nonce. */
static uint8_t evil_pk[PK_BYTES];

static bool swap_keys(int from, int to, uint8_t *buf, size_t *len)
{
	int kind = lsec_frame_kind(buf, *len);

	if (kind == KIND_PAIR_REQ) {
		memcpy(buf + PAIR_HDR + 4, evil_pk, PK_BYTES);
	} else if (kind == KIND_PAIR_RSP) {
		memcpy(buf + PAIR_HDR, evil_pk, PK_BYTES);
	}
	return true;
}

ZTEST(lsec, test_key_swap_breaks_commitment)
{
	uint32_t id = 0;

	zassert_ok(lsec_ecdh_generate(&id, evil_pk));
	lsec_ecdh_destroy(&id);

	filter = swap_keys;
	pair_to_sas(N_SAT1, 1);

	zassert_equal(nodes[N_CTRL].sas_count, 0);
	zassert_equal(nodes[N_CTRL].fail, LSEC_FAIL_COMMITMENT);
	/* The controller's ABORT ends the satellite's pairing mode too. */
	zassert_equal(nodes[N_SAT1].fail, LSEC_FAIL_PEER_ABORT);
	zassert_false(lsec_sat_pairing_active(&sat1));
}

static bool flip_ns(int from, int to, uint8_t *buf, size_t *len)
{
	if (lsec_frame_kind(buf, *len) == KIND_PAIR_NS) {
		buf[PAIR_HDR] ^= 1;
	}
	return true;
}

ZTEST(lsec, test_bad_commitment)
{
	filter = flip_ns;
	pair_to_sas(N_SAT1, 1);

	zassert_equal(nodes[N_CTRL].fail, LSEC_FAIL_COMMITMENT);
	zassert_false(lsec_ctrl_pairing_active(&ctrl));
}

static bool flip_ec(int from, int to, uint8_t *buf, size_t *len)
{
	if (lsec_frame_kind(buf, *len) == KIND_PAIR_CHK_C) {
		buf[PAIR_HDR + 1] ^= 1;
	}
	return true;
}

ZTEST(lsec, test_bad_key_confirmation)
{
	pair_to_sas(N_SAT1, 1);
	filter = flip_ec;
	lsec_sat_pair_confirm(&sat1, true, now);
	lsec_ctrl_pair_confirm(&ctrl, true, now);
	pump();

	zassert_equal(nodes[N_SAT1].fail, LSEC_FAIL_CHECK);
	zassert_equal(nodes[N_CTRL].fail, LSEC_FAIL_PEER_ABORT);
	zassert_false(nodes[N_SAT1].has_stored[0]);
	zassert_false(nodes[N_CTRL].has_stored[1]);
}

static bool change_slot(int from, int to, uint8_t *buf, size_t *len)
{
	if (lsec_frame_kind(buf, *len) == KIND_PAIR_CHK_C) {
		buf[PAIR_HDR] = 2;
	}
	return true;
}

ZTEST(lsec, test_slot_is_authenticated)
{
	pair_to_sas(N_SAT1, 1);
	filter = change_slot;
	lsec_sat_pair_confirm(&sat1, true, now);
	lsec_ctrl_pair_confirm(&ctrl, true, now);
	pump();

	zassert_equal(nodes[N_SAT1].fail, LSEC_FAIL_CHECK);
	zassert_false(nodes[N_SAT1].has_stored[0]);
}

ZTEST(lsec, test_reject_and_timeout)
{
	/* Satellite user says no. */
	pair_to_sas(N_SAT1, 1);
	lsec_sat_pair_confirm(&sat1, false, now);
	pump();
	zassert_equal(nodes[N_SAT1].fail, LSEC_FAIL_REJECTED);
	zassert_equal(nodes[N_CTRL].fail, LSEC_FAIL_PEER_ABORT);
	zassert_false(lsec_sat_pairing_active(&sat1));
	zassert_false(lsec_ctrl_pairing_active(&ctrl));

	/* Controller user says no. */
	setup(NULL);
	pair_to_sas(N_SAT1, 1);
	lsec_ctrl_pair_confirm(&ctrl, false, now);
	pump();
	zassert_equal(nodes[N_CTRL].fail, LSEC_FAIL_REJECTED);
	zassert_equal(nodes[N_SAT1].fail, LSEC_FAIL_PEER_ABORT);

	/* Nobody confirms. */
	setup(NULL);
	pair_to_sas(N_SAT1, 1);
	tick(LSEC_PAIR_TIMEOUT_MS + 1000);
	zassert_equal(nodes[N_CTRL].fail, LSEC_FAIL_TIMEOUT);
	zassert_false(lsec_ctrl_pairing_active(&ctrl));
	zassert_false(lsec_sat_pairing_active(&sat1));

	/* Pairing mode closes by itself when no controller comes. */
	setup(NULL);
	zassert_ok(lsec_sat_pair_start(&sat1, now));
	tick(LSEC_PAIR_WINDOW_MS + 1000);
	zassert_equal(nodes[N_SAT1].fail, LSEC_FAIL_TIMEOUT);
	zassert_false(lsec_sat_pairing_active(&sat1));

	for (int i = 0; i <= LSEC_MAX_SATS; i++) {
		zassert_false(nodes[N_CTRL].has_stored[i]);
	}
	zassert_false(nodes[N_SAT1].has_stored[0]);
}

ZTEST(lsec, test_satellite_ignores_pairing_when_not_in_pairing_mode)
{
	zassert_ok(lsec_ctrl_pair_start(&ctrl, UID1, 1, now));
	pump();
	zassert_equal(nodes[N_SAT1].sas_count, 0);
	zassert_true(sat1.stats.foreign > 0);
}

ZTEST(lsec, test_replay_tamper_foreign)
{
	static const char msg[] = "FIRE";
	struct frame f;

	pair(N_SAT1, 1);
	join(N_SAT1, 1);

	zassert_ok(lsec_ctrl_send(&ctrl, 1, msg, sizeof(msg)));
	pump();
	zassert_equal(nodes[N_SAT1].datas, 1);

	/* Replay. */
	f = captured[N_CTRL][KIND_DATA];
	inject(N_SAT1, &f);
	zassert_equal(nodes[N_SAT1].datas, 1);
	zassert_equal(sat1.stats.replay, 1);

	/* A fresh counter does not help without the key. */
	f.buf[6] += 5;
	inject(N_SAT1, &f);
	zassert_equal(nodes[N_SAT1].datas, 1);
	zassert_equal(sat1.stats.bad_tag, 1);

	/* Nor does changing the kind, which is covered by the tag. */
	f = captured[N_CTRL][KIND_DATA];
	f.buf[0] = lsec_kind_byte(KIND_GROUP_UPDATE);
	f.buf[6] += 7;
	inject(N_SAT1, &f);
	zassert_equal(sat1.stats.bad_tag, 2);

	/* Another controller's network is dropped before any crypto. */
	f = captured[N_CTRL][KIND_DATA];
	f.buf[1] ^= 0xFF;
	inject(N_SAT1, &f);
	zassert_equal(sat1.stats.foreign, 1);

	/* A replayed satellite frame at the controller. */
	zassert_ok(lsec_sat_send(&sat1, msg, sizeof(msg)));
	pump();
	zassert_equal(nodes[N_CTRL].datas, 1);
	f = captured[N_SAT1][KIND_DATA];
	inject(N_CTRL, &f);
	zassert_equal(nodes[N_CTRL].datas, 1);
	zassert_equal(ctrl.stats.replay, 1);

	/* Still works afterwards. */
	zassert_ok(lsec_ctrl_send(&ctrl, 1, msg, sizeof(msg)));
	pump();
	zassert_equal(nodes[N_SAT1].datas, 2);
}

ZTEST(lsec, test_spoofed_join_req_keeps_session)
{
	static const char msg[] = "HB";
	struct frame f;

	pair(N_SAT1, 1);
	join(N_SAT1, 1);

	/* An old JOIN_REQ, replayed, and a HELLO forged at the controller. */
	f = captured[N_CTRL][KIND_JOIN_REQ];
	inject(N_SAT1, &f);
	zassert_true(lsec_sat_joined(&sat1));

	zassert_ok(lsec_ctrl_send(&ctrl, 1, msg, sizeof(msg)));
	pump();
	zassert_equal(nodes[N_SAT1].datas, 1);

	/* A replayed JOIN_RSP to that request is refused as well. */
	f = captured[N_SAT1][KIND_JOIN_RSP];
	inject(N_CTRL, &f);
	zassert_true(lsec_ctrl_joined(&ctrl, 1));
	zassert_ok(lsec_sat_send(&sat1, msg, sizeof(msg)));
	pump();
	zassert_equal(nodes[N_CTRL].datas, 1);
}

ZTEST(lsec, test_controller_reboot_needs_new_join)
{
	static const char msg[] = "HB";
	struct lsec_pairing p;
	struct frame stale;

	pair(N_SAT1, 1);
	p = nodes[N_CTRL].stored[1];
	join(N_SAT1, 1);

	/* A frame captured in the old session but never delivered. */
	nodes[N_SAT1].up = false;
	zassert_ok(lsec_ctrl_send(&ctrl, 1, msg, sizeof(msg)));
	pump();
	stale = captured[N_CTRL][KIND_DATA];
	nodes[N_SAT1].up = true;

	/* Reboot: same network and pairing, no session. */
	struct node saved = nodes[N_CTRL];

	node_init(N_CTRL, 0);
	memcpy(nodes[N_CTRL].stored, saved.stored, sizeof(saved.stored));
	zassert_ok(lsec_ctrl_load(&ctrl, &p));
	zassert_false(lsec_ctrl_joined(&ctrl, 1));
	zassert_equal(lsec_ctrl_send(&ctrl, 1, msg, sizeof(msg)), -ENOTCONN);

	/* The satellite notices the silence and asks to join again. */
	sat1.session_timeout_ms = LSEC_SESSION_TIMEOUT_MS;
	tick(LSEC_SESSION_TIMEOUT_MS + LSEC_HELLO_MS);
	join(N_SAT1, 1);

	int before = nodes[N_SAT1].datas;

	inject(N_SAT1, &stale);
	zassert_equal(nodes[N_SAT1].datas, before, "old-session frame accepted");

	zassert_ok(lsec_ctrl_send(&ctrl, 1, msg, sizeof(msg)));
	pump();
	zassert_equal(nodes[N_SAT1].datas, before + 1);
}

ZTEST(lsec, test_satellite_reboot_rejoins)
{
	static const char msg[] = "HB";
	struct lsec_pairing p;

	pair(N_SAT1, 1);
	join(N_SAT1, 1);
	p = nodes[N_SAT1].stored[0];

	node_init(N_SAT1, UID1);
	zassert_ok(lsec_sat_load(&sat1, &p));

	/* No session yet: controller data is refused. */
	zassert_ok(lsec_ctrl_send(&ctrl, 1, msg, sizeof(msg)));
	pump();
	zassert_equal(nodes[N_SAT1].datas, 0);
	zassert_equal(sat1.stats.no_session, 1);

	join(N_SAT1, 1);
	zassert_ok(lsec_ctrl_send(&ctrl, 1, msg, sizeof(msg)));
	pump();
	zassert_equal(nodes[N_SAT1].datas, 1);
}

ZTEST(lsec, test_group_and_forget)
{
	static const char all[] = "SAFE ALL";
	struct frame old_group;

	pair(N_SAT1, 1);
	pair(N_SAT2, 2);
	join(N_SAT1, 1);
	join(N_SAT2, 2);

	zassert_ok(lsec_ctrl_send(&ctrl, LSEC_ADDR_GROUP, all, sizeof(all)));
	pump();
	zassert_equal(nodes[N_SAT1].datas, 1);
	zassert_equal(nodes[N_SAT2].datas, 1);
	zassert_true(nodes[N_SAT1].data_group);
	zassert_true(nodes[N_SAT2].data_group);
	old_group = captured[N_CTRL][KIND_DATA];

	/* A satellite cannot pass a unicast frame to the other off as its own. */
	zassert_ok(lsec_ctrl_send(&ctrl, 1, all, sizeof(all)));
	pump();
	zassert_equal(nodes[N_SAT1].datas, 2);
	zassert_equal(nodes[N_SAT2].datas, 1);

	/* Forget satellite 2: satellite 1 moves to a new group key. */
	zassert_ok(lsec_ctrl_forget(&ctrl, 2, now));
	pump();
	zassert_false(nodes[N_CTRL].has_stored[2]);
	zassert_false(ctrl.peer[1].gtk_pending, "GROUP_UPDATE not acked");

	zassert_ok(lsec_ctrl_send(&ctrl, LSEC_ADDR_GROUP, all, sizeof(all)));
	pump();
	zassert_equal(nodes[N_SAT1].datas, 3);
	zassert_equal(nodes[N_SAT2].datas, 1, "forgotten satellite read the new group key");

	/* The old group frame is worthless to satellite 1 now. */
	inject(N_SAT1, &old_group);
	zassert_equal(nodes[N_SAT1].datas, 3);
}

static int drops;

static bool drop_updates(int from, int to, uint8_t *buf, size_t *len)
{
	if (lsec_frame_kind(buf, *len) == KIND_GROUP_UPDATE && drops > 0) {
		drops--;
		return false;
	}
	return true;
}

ZTEST(lsec, test_group_update_survives_loss)
{
	static const char all[] = "HB";

	drops = 2;
	pair(N_SAT1, 1);
	pair(N_SAT2, 2);
	join(N_SAT1, 1);
	join(N_SAT2, 2);

	filter = drop_updates;
	zassert_ok(lsec_ctrl_forget(&ctrl, 2, now));
	pump();
	zassert_true(ctrl.peer[1].gtk_pending);
	tick(3 * LSEC_RESEND_MS);
	zassert_false(ctrl.peer[1].gtk_pending);

	zassert_ok(lsec_ctrl_send(&ctrl, LSEC_ADDR_GROUP, all, sizeof(all)));
	pump();
	zassert_equal(nodes[N_SAT1].datas, 1);
}

ZTEST(lsec, test_repairing_replaces)
{
	/* Pair satellite 1 into slot 1, then again into slot 3. */
	pair(N_SAT1, 1);
	nodes[N_SAT1].paired = 0;
	nodes[N_SAT1].sas_count = 0;
	pair(N_SAT1, 3);

	zassert_false(nodes[N_CTRL].has_stored[1], "old slot kept");
	zassert_true(nodes[N_CTRL].has_stored[3]);
	zassert_is_null(lsec_ctrl_pairing(&ctrl, 1));
	zassert_equal(nodes[N_SAT1].stored[0].slot, 3);
	join(N_SAT1, 3);
}

ZTEST(lsec, test_sas_layout)
{
	pair_to_sas(N_SAT1, 1);

	for (int i = 0; i < LSEC_SAS_FRAMES; i++) {
		zassert_true(nodes[N_CTRL].sas[i] < (1 << LSEC_SAS_LEDS));
	}
}

/* ---- polls and session timeouts ------------------------------------ */

#define SLOT_BIT(slot) (1u << ((slot) - 1))
#define TEST_SLOT_MS   40

static void send_poll(uint8_t mask, const void *data, size_t len)
{
	zassert_ok(lsec_ctrl_send_poll(&ctrl, mask, TEST_SLOT_MS, data, len));
	pump();
}

ZTEST(lsec, test_poll_ranks)
{
	static const char cmds[] = "abcd";
	uint8_t mask = SLOT_BIT(1) | SLOT_BIT(2) | SLOT_BIT(4) | SLOT_BIT(5);

	pair(N_SAT1, 2);
	pair(N_SAT2, 5);
	join(N_SAT1, 2);
	join(N_SAT2, 5);

	send_poll(mask, cmds, sizeof(cmds));

	/* Slots 1 and 4 are not paired; the ranks still count them. */
	zassert_equal(nodes[N_SAT1].polls, 1);
	zassert_equal(nodes[N_SAT1].poll.rank, 1);
	zassert_equal(nodes[N_SAT1].poll.count, 4);
	zassert_equal(nodes[N_SAT1].poll.slot_ms, TEST_SLOT_MS);
	zassert_equal(nodes[N_SAT1].poll.len, sizeof(cmds));
	zassert_mem_equal(nodes[N_SAT1].poll_data, cmds, sizeof(cmds));

	zassert_equal(nodes[N_SAT2].polls, 1);
	zassert_equal(nodes[N_SAT2].poll.rank, 3);
	zassert_equal(nodes[N_SAT2].poll.count, 4);

	/* A poll is not application data. */
	zassert_equal(nodes[N_SAT1].datas, 0);
}

ZTEST(lsec, test_poll_unaddressed_silent)
{
	struct frame f;
	uint32_t ok;

	pair(N_SAT1, 1);
	join(N_SAT1, 1);
	ok = sat1.stats.rx_ok;

	send_poll(SLOT_BIT(2), NULL, 0);
	zassert_equal(nodes[N_SAT1].polls, 0);
	zassert_equal(nodes[N_SAT1].datas, 0);
	zassert_equal(sat1.stats.rx_ok, ok + 1, "poll not opened");

	/* The window moved all the same. */
	f = captured[N_CTRL][KIND_POLL];
	inject(N_SAT1, &f);
	zassert_equal(sat1.stats.replay, 1);
}

ZTEST(lsec, test_polls_keep_session)
{
	pair(N_SAT1, 1);
	join(N_SAT1, 1);
	sat1.session_timeout_ms = LSEC_SESSION_TIMEOUT_MS;
	memset(&captured[N_SAT1][KIND_HELLO], 0, sizeof(struct frame));

	for (int64_t t = 0; t < 3 * LSEC_SESSION_TIMEOUT_MS; t += 300) {
		send_poll(SLOT_BIT(1), NULL, 0);
		tick(300);
	}

	zassert_true(lsec_sat_joined(&sat1));
	zassert_equal(nodes[N_SAT1].lefts, 0);
	zassert_equal(captured[N_SAT1][KIND_HELLO].len, 0, "HELLO while being polled");
}

ZTEST(lsec, test_unaddressed_times_out)
{
	static const char all[] = "HB";

	pair(N_SAT1, 1);
	join(N_SAT1, 1);
	sat1.session_timeout_ms = LSEC_SESSION_TIMEOUT_MS;
	memset(&captured[N_SAT1][KIND_HELLO], 0, sizeof(struct frame));

	/*
	 * The controller is alive and broadcasting, but no longer asks this
	 * satellite: that must end its session.
	 */
	for (int64_t t = 0; t < LSEC_SESSION_TIMEOUT_MS + 300; t += 300) {
		send_poll(SLOT_BIT(2), NULL, 0);
		zassert_ok(lsec_ctrl_send(&ctrl, LSEC_ADDR_GROUP, all, sizeof(all)));
		pump();
		tick(300);
	}

	zassert_equal(nodes[N_SAT1].lefts, 1);
	zassert_true(captured[N_SAT1][KIND_HELLO].len > 0, "no HELLO after the timeout");

	/* The HELLO brings a new join. */
	join(N_SAT1, 1);
	send_poll(SLOT_BIT(1), NULL, 0);
	zassert_equal(nodes[N_SAT1].polls, 1);
}

ZTEST(lsec, test_poll_tamper)
{
	struct frame f;
	struct lsec_dir d;
	uint8_t body[LSEC_POLL_HDR] = {SLOT_BIT(1), 0};
	int n;

	pair(N_SAT1, 1);
	join(N_SAT1, 1);
	send_poll(SLOT_BIT(1), NULL, 0);
	zassert_equal(nodes[N_SAT1].polls, 1);

	/* The mask is under the tag. */
	f = captured[N_CTRL][KIND_POLL];
	f.buf[SEAL_HDR] ^= SLOT_BIT(2);
	f.buf[6] += 1;
	inject(N_SAT1, &f);
	zassert_equal(sat1.stats.bad_tag, 1);

	/* A poll is never unicast. */
	f = captured[N_CTRL][KIND_POLL];
	f.buf[4] = 1;
	f.buf[6] += 2;
	inject(N_SAT1, &f);
	zassert_equal(sat1.stats.malformed, 1);

	/* Validly sealed but broken bodies: no slot length, then no mask. */
	d = ctrl.group.dir;
	n = lsec_seal(&d, KIND_POLL, NET, LSEC_ADDR_CTRL, LSEC_ADDR_GROUP, ctrl.group.kid, body,
		      sizeof(body), f.buf);
	zassert_true(n > 0);
	f.len = n;
	inject(N_SAT1, &f);
	zassert_equal(sat1.stats.malformed, 2);

	n = lsec_seal(&d, KIND_POLL, NET, LSEC_ADDR_CTRL, LSEC_ADDR_GROUP, ctrl.group.kid, body, 1,
		      f.buf);
	f.len = n;
	inject(N_SAT1, &f);
	zassert_equal(sat1.stats.malformed, 3);
	zassert_equal(nodes[N_SAT1].polls, 1);
}

ZTEST(lsec, test_poll_shares_group_counter)
{
	static const char all[] = "HB";
	struct frame first;

	pair(N_SAT1, 1);
	join(N_SAT1, 1);

	send_poll(SLOT_BIT(1), NULL, 0);
	first = captured[N_CTRL][KIND_POLL];
	zassert_ok(lsec_ctrl_send(&ctrl, LSEC_ADDR_GROUP, all, sizeof(all)));
	pump();
	send_poll(SLOT_BIT(1), NULL, 0);

	zassert_equal(nodes[N_SAT1].polls, 2);
	zassert_equal(nodes[N_SAT1].datas, 1);

	inject(N_SAT1, &first);
	zassert_equal(nodes[N_SAT1].polls, 2);
	zassert_equal(sat1.stats.replay, 1);
}

ZTEST(lsec, test_poll_after_rotation)
{
	pair(N_SAT1, 1);
	pair(N_SAT2, 2);
	join(N_SAT1, 1);
	join(N_SAT2, 2);

	zassert_ok(lsec_ctrl_forget(&ctrl, 2, now));
	pump();

	/* Even with its bit set, the forgotten satellite cannot read the poll. */
	send_poll(SLOT_BIT(1) | SLOT_BIT(2), NULL, 0);
	zassert_equal(nodes[N_SAT1].polls, 1);
	zassert_equal(nodes[N_SAT1].poll.rank, 0);
	zassert_equal(nodes[N_SAT2].polls, 0);
}

ZTEST(lsec, test_send_poll_args)
{
	static uint8_t big[LSEC_MAX_POLL_PAYLOAD + 1];

	zassert_equal(lsec_ctrl_send_poll(&ctrl, 0, TEST_SLOT_MS, NULL, 0), -EINVAL);
	zassert_equal(lsec_ctrl_send_poll(&ctrl, SLOT_BIT(1), 0, NULL, 0), -EINVAL);
	zassert_equal(lsec_ctrl_send_poll(&ctrl, SLOT_BIT(1), TEST_SLOT_MS, big, sizeof(big)),
		      -EMSGSIZE);
	zassert_ok(lsec_ctrl_send_poll(&ctrl, SLOT_BIT(1), TEST_SLOT_MS, big, sizeof(big) - 1));

	/* Nobody is joined, and the poll still goes out as asked. */
	zassert_equal(lsec_ctrl_joined_mask(&ctrl), 0);
	zassert_ok(lsec_ctrl_send_poll(&ctrl, 0xFF, TEST_SLOT_MS, NULL, 0));
	zassert_equal(lsec_frame_kind(captured[N_CTRL][KIND_POLL].buf,
				      captured[N_CTRL][KIND_POLL].len), KIND_POLL);
}

ZTEST(lsec, test_ctrl_peer_timeout)
{
	static const char msg[] = "HB";

	pair(N_SAT1, 1);
	join(N_SAT1, 1);
	zassert_equal(lsec_ctrl_joined_mask(&ctrl), SLOT_BIT(1));
	ctrl.peer_timeout_ms = LSEC_PEER_TIMEOUT_MS;

	nodes[N_SAT1].up = false;
	tick(LSEC_PEER_TIMEOUT_MS + 200);

	zassert_equal(nodes[N_CTRL].lefts, 1);
	zassert_false(lsec_ctrl_joined(&ctrl, 1));
	zassert_equal(lsec_ctrl_joined_mask(&ctrl), 0);
	zassert_not_null(lsec_ctrl_pairing(&ctrl, 1), "pairing lost with the session");
	zassert_equal(lsec_ctrl_send(&ctrl, 1, msg, sizeof(msg)), -ENOTCONN);

	/* It rejoins from the controller's side once it can hear again. */
	nodes[N_SAT1].up = true;
	tick(LSEC_JOIN_RETRY_MS);
	join(N_SAT1, 1);
	zassert_ok(lsec_ctrl_send(&ctrl, 1, msg, sizeof(msg)));
	pump();
	zassert_equal(nodes[N_SAT1].datas, 1);
}

ZTEST(lsec, test_replies_keep_peer)
{
	pair(N_SAT1, 1);
	join(N_SAT1, 1);
	ctrl.peer_timeout_ms = LSEC_PEER_TIMEOUT_MS;
	nodes[N_SAT1].auto_reply = true;

	for (int64_t t = 0; t < 3 * LSEC_PEER_TIMEOUT_MS; t += 300) {
		send_poll(SLOT_BIT(1), NULL, 0);
		tick(300);
	}

	zassert_equal(nodes[N_CTRL].lefts, 0);
	zassert_true(lsec_ctrl_joined(&ctrl, 1));
	/* The empty replies arrive as DATA. */
	zassert_true(nodes[N_CTRL].datas > 0);
	zassert_equal(nodes[N_CTRL].data_len, 0);
}

ZTEST_SUITE(lsec, NULL, NULL, setup, NULL, NULL);
