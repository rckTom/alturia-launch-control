/* SPDX-License-Identifier: Apache-2.0 */

/* Controller role: pairing initiator, join initiator, group key owner. */

#include <errno.h>
#include <string.h>
#include <psa/crypto.h>

#include "lsec_internal.h"

enum ctrl_pair_state {
	CP_IDLE,
	CP_REQ,      /* PAIR_REQ sent */
	CP_NC,       /* PAIR_NC sent */
	CP_CONFIRM,  /* SAS shown, waiting for the user */
	CP_CHK,      /* PAIR_CHK_C sent, waiting for the satellite's user and Es */
};

#define OK_TRIES 3

_Static_assert(LSEC_MAX_SATS <= 8, "a poll's mask has one bit per slot");

static void tx(struct lsec_ctrl *c, const uint8_t *buf, size_t len)
{
	c->io.send(c->io.user, buf, len);
}

static void pair_tx(struct lsec_ctrl *c, const uint8_t *buf, size_t len, int64_t now)
{
	memcpy(c->pair.last_frame, buf, len);
	c->pair.last_len = len;
	c->pair.last_tx = lsec_jittered(now, LSEC_PAIR_RETRY_MS);
	tx(c, buf, len);
}

static bool valid_slot(uint8_t slot)
{
	return slot >= 1 && slot <= LSEC_MAX_SATS;
}

static void peer_clear(struct lsec_ctrl_peer *peer)
{
	lsec_wipe(peer, sizeof(*peer));
	peer->hs.sent = LSEC_NEVER;
	peer->join_retry_from = LSEC_NEVER;
}

/* End the session but keep the pairing, so the satellite can join again. */
static void peer_drop_session(struct lsec_ctrl_peer *peer)
{
	peer->joined = false;
	peer->ok_pending = false;
	peer->gtk_pending = false;
	peer->ok_tries = 0;
	lsec_wipe(&peer->hs, sizeof(peer->hs));
	lsec_wipe(&peer->tx, sizeof(peer->tx));
	lsec_wipe(&peer->rx, sizeof(peer->rx));
	/* Start the rejoin at once rather than after JOIN_RETRY. */
	peer->hs.sent = LSEC_NEVER;
	peer->join_retry_from = LSEC_NEVER;
}

static void send_sealed(struct lsec_ctrl *c, uint8_t slot, enum lsec_kind kind,
			const void *data, size_t len)
{
	uint8_t out[LSEC_MAX_FRAME];
	int n;

	n = lsec_seal(&c->peer[slot].tx, kind, c->net_id, LSEC_ADDR_CTRL, slot, 0, data, len,
		      out);
	if (n > 0) {
		tx(c, out, n);
	}
}

static void send_group_update(struct lsec_ctrl *c, uint8_t slot, int64_t now)
{
	uint8_t body[GTK_BODY];

	lsec_group_body(&c->group, body);
	send_sealed(c, slot, KIND_GROUP_UPDATE, body, sizeof(body));
	c->peer[slot].gtk_sent = lsec_jittered(now, LSEC_RESEND_MS);
	lsec_wipe(body, sizeof(body));
}

/* New group key: the old one may be held by a satellite that is gone. */
static int group_rotate(struct lsec_ctrl *c, int64_t now)
{
	uint8_t kid = c->group.valid ? c->group.kid + 1 : 0;

	lsec_wipe(&c->group, sizeof(c->group));
	if (lsec_random(c->group.dir.key, LSEC_KEY_BYTES)) {
		return -EIO;
	}
	c->group.kid = kid;
	c->group.valid = true;

	for (uint8_t slot = 1; slot <= LSEC_MAX_SATS; slot++) {
		struct lsec_ctrl_peer *peer = &c->peer[slot];

		if (peer->joined) {
			peer->gtk_pending = true;
			send_group_update(c, slot, now);
		}
	}

	return 0;
}

int lsec_ctrl_init(struct lsec_ctrl *c, const struct lsec_io *io, uint32_t net_id)
{
	int ret;

	memset(c, 0, sizeof(*c));
	c->io = *io;
	c->net_id = net_id;
	c->pair.key_id = PSA_KEY_ID_NULL;
	c->peer_timeout_ms = LSEC_PEER_TIMEOUT_MS;

	for (uint8_t slot = 0; slot <= LSEC_MAX_SATS; slot++) {
		peer_clear(&c->peer[slot]);
	}

	ret = lsec_crypto_init();
	if (ret) {
		return ret;
	}

	return group_rotate(c, 0);
}

int lsec_ctrl_load(struct lsec_ctrl *c, const struct lsec_pairing *p)
{
	struct lsec_ctrl_peer *peer;

	if (!valid_slot(p->slot) || p->net_id != c->net_id) {
		return -EINVAL;
	}

	peer = &c->peer[p->slot];
	peer_clear(peer);
	peer->pairing = *p;
	peer->paired = true;

	return 0;
}

bool lsec_ctrl_joined(const struct lsec_ctrl *c, uint8_t slot)
{
	return valid_slot(slot) && c->peer[slot].joined;
}

uint8_t lsec_ctrl_joined_mask(const struct lsec_ctrl *c)
{
	uint8_t mask = 0;

	for (uint8_t slot = 1; slot <= LSEC_MAX_SATS; slot++) {
		if (c->peer[slot].joined) {
			mask |= 1u << (slot - 1);
		}
	}

	return mask;
}

const struct lsec_pairing *lsec_ctrl_pairing(const struct lsec_ctrl *c, uint8_t slot)
{
	if (!valid_slot(slot) || !c->peer[slot].paired) {
		return NULL;
	}

	return &c->peer[slot].pairing;
}

int lsec_ctrl_forget(struct lsec_ctrl *c, uint8_t slot, int64_t now)
{
	int ret = 0;

	if (!valid_slot(slot) || !c->peer[slot].paired) {
		return -ENOENT;
	}

	if (c->io.store) {
		ret = c->io.store(c->io.user, slot, NULL);
	}

	peer_clear(&c->peer[slot]);
	group_rotate(c, now);

	return ret;
}

bool lsec_ctrl_pairing_active(const struct lsec_ctrl *c)
{
	return c->pair.state != CP_IDLE;
}

void lsec_ctrl_scan(struct lsec_ctrl *c, bool on)
{
	c->scanning = on;
}

/* ---- pairing ---------------------------------------------------------- */

static void pair_abort(struct lsec_ctrl *c, enum lsec_fail reason, bool tell_peer)
{
	uint64_t uid = c->pair.uid;

	if (tell_peer) {
		uint8_t buf[PAIR_HDR + 1];

		lsec_pair_hdr(buf, KIND_PAIR_ABORT, uid);
		buf[PAIR_HDR] = reason;
		tx(c, buf, sizeof(buf));
	}

	lsec_pair_reset(&c->pair);
	lsec_emit_fail(&c->io, uid, reason);
}

int lsec_ctrl_pair_start(struct lsec_ctrl *c, uint64_t uid, uint8_t slot, int64_t now)
{
	struct lsec_pair *p = &c->pair;
	uint8_t buf[PAIR_HDR + 4 + PK_BYTES];

	if (!valid_slot(slot)) {
		return -EINVAL;
	}

	lsec_pair_reset(p);
	p->uid = uid;
	p->slot = slot;
	p->net_id = c->net_id;
	p->started = now;

	if (lsec_ecdh_generate(&p->key_id, p->pk_own) || lsec_random(p->n_own, NONCE_PAIR)) {
		lsec_pair_reset(p);
		return -EIO;
	}

	lsec_pair_hdr(buf, KIND_PAIR_REQ, uid);
	put_le32(&buf[PAIR_HDR], c->net_id);
	memcpy(&buf[PAIR_HDR + 4], p->pk_own, PK_BYTES);
	pair_tx(c, buf, sizeof(buf), now);
	p->state = CP_REQ;

	return 0;
}

void lsec_ctrl_pair_confirm(struct lsec_ctrl *c, bool accept, int64_t now)
{
	struct lsec_pair *p = &c->pair;
	uint8_t buf[PAIR_HDR + 1 + CHECK_BYTES];

	if (p->state == CP_IDLE) {
		return;
	}

	if (!accept) {
		pair_abort(c, LSEC_FAIL_REJECTED, true);
		return;
	}

	if (p->state != CP_CONFIRM) {
		return;
	}

	lsec_pair_hdr(buf, KIND_PAIR_CHK_C, p->uid);
	buf[PAIR_HDR] = p->slot;
	if (lsec_pair_check(p, true, true, &buf[PAIR_HDR + 1])) {
		pair_abort(c, LSEC_FAIL_CRYPTO, true);
		return;
	}

	p->user_ok = true;
	pair_tx(c, buf, sizeof(buf), now);
	p->state = CP_CHK;
}

static void pair_finish(struct lsec_ctrl *c, int64_t now)
{
	struct lsec_pair *p = &c->pair;
	struct lsec_pairing rec = {
		.sat_uid = p->uid,
		.net_id = c->net_id,
		.slot = p->slot,
	};
	bool replaced = false;
	int ret;

	memcpy(rec.ltk, p->ltk, sizeof(rec.ltk));

	ret = c->io.store ? c->io.store(c->io.user, p->slot, &rec) : 0;
	if (ret) {
		lsec_wipe(&rec, sizeof(rec));
		pair_abort(c, LSEC_FAIL_STORE, false);
		return;
	}

	/* A satellite has one slot, and a slot one satellite. */
	for (uint8_t slot = 1; slot <= LSEC_MAX_SATS; slot++) {
		struct lsec_ctrl_peer *peer = &c->peer[slot];

		if (!peer->paired) {
			continue;
		}
		if (slot == p->slot || peer->pairing.sat_uid == p->uid) {
			if (slot != p->slot && c->io.store) {
				c->io.store(c->io.user, slot, NULL);
			}
			peer_clear(peer);
			replaced = true;
		}
	}

	c->peer[p->slot].pairing = rec;
	c->peer[p->slot].paired = true;
	lsec_wipe(&rec, sizeof(rec));

	if (replaced) {
		group_rotate(c, now);
	}

	struct lsec_event ev = {
		.type = LSEC_EV_PAIRED,
		.slot = p->slot,
		.uid = p->uid,
	};

	lsec_pair_reset(p);
	lsec_emit(&c->io, &ev);
}

static void ctrl_pair_rx(struct lsec_ctrl *c, int kind, const uint8_t *buf, size_t len,
			 int16_t rssi, int64_t now)
{
	struct lsec_pair *p = &c->pair;
	const uint8_t *body = buf + PAIR_HDR;
	size_t body_len = len - PAIR_HDR;
	uint64_t uid;
	uint8_t out[PAIR_HDR + NONCE_PAIR];

	if (len < PAIR_HDR) {
		c->stats.malformed++;
		return;
	}

	uid = get_le64(&buf[1]);

	if (kind == KIND_PAIR_ADV) {
		if (c->scanning && body_len == 0) {
			struct lsec_event ev = {.type = LSEC_EV_ADV, .uid = uid, .rssi_dbm = rssi};

			lsec_emit(&c->io, &ev);
		}
		return;
	}

	if (p->state == CP_IDLE || uid != p->uid) {
		c->stats.foreign++;
		return;
	}

	switch (kind) {
	case KIND_PAIR_RSP:
		if (body_len != PK_BYTES + CHECK_BYTES) {
			break;
		}
		if (p->state != CP_REQ) {
			return;
		}
		memcpy(p->pk_peer, body, PK_BYTES);
		memcpy(p->commit, body + PK_BYTES, CHECK_BYTES);

		lsec_pair_hdr(out, KIND_PAIR_NC, uid);
		memcpy(out + PAIR_HDR, p->n_own, NONCE_PAIR);
		pair_tx(c, out, sizeof(out), now);
		p->state = CP_NC;
		c->stats.rx_ok++;
		return;

	case KIND_PAIR_NS: {
		struct lsec_event ev = {.type = LSEC_EV_SAS, .uid = uid, .slot = p->slot};
		uint8_t commit[CHECK_BYTES];

		if (body_len != NONCE_PAIR) {
			break;
		}
		if (p->state != CP_NC) {
			return;
		}
		memcpy(p->n_peer, body, NONCE_PAIR);

		if (lsec_pair_commitment(p->n_peer, p->pk_peer, p->pk_own, commit)) {
			pair_abort(c, LSEC_FAIL_CRYPTO, true);
			return;
		}
		if (!lsec_equal(commit, p->commit, CHECK_BYTES)) {
			pair_abort(c, LSEC_FAIL_COMMITMENT, true);
			return;
		}
		if (lsec_pair_derive(p, true, ev.sas)) {
			pair_abort(c, LSEC_FAIL_CRYPTO, true);
			return;
		}

		p->state = CP_CONFIRM;
		c->stats.rx_ok++;
		lsec_emit(&c->io, &ev);
		return;
	}

	case KIND_PAIR_CHK_S: {
		uint8_t expect[CHECK_BYTES];

		if (body_len != CHECK_BYTES) {
			break;
		}
		if (p->state != CP_CHK) {
			return;
		}
		if (lsec_pair_check(p, true, false, expect)) {
			pair_abort(c, LSEC_FAIL_CRYPTO, true);
			return;
		}
		if (!lsec_equal(expect, body, CHECK_BYTES)) {
			pair_abort(c, LSEC_FAIL_CHECK, true);
			return;
		}

		c->stats.rx_ok++;
		pair_finish(c, now);
		return;
	}

	case KIND_PAIR_ABORT:
		pair_abort(c, LSEC_FAIL_PEER_ABORT, false);
		return;

	default:
		return;
	}

	c->stats.malformed++;
}

/* ---- session ---------------------------------------------------------- */

static void join_start(struct lsec_ctrl *c, uint8_t slot, int64_t now)
{
	struct lsec_ctrl_peer *peer = &c->peer[slot];
	uint8_t buf[LINK_HDR + NONCE_JOIN];

	if (lsec_random(peer->hs.nc, NONCE_JOIN)) {
		return;
	}

	peer->hs.pending = true;
	peer->hs.sent = now;
	peer->join_retry_from = lsec_jittered(now, LSEC_JOIN_RETRY_MS);

	lsec_link_hdr(buf, KIND_JOIN_REQ, c->net_id, LSEC_ADDR_CTRL, slot);
	memcpy(buf + LINK_HDR, peer->hs.nc, NONCE_JOIN);
	tx(c, buf, sizeof(buf));
}

static void send_join_ok(struct lsec_ctrl *c, uint8_t slot, int64_t now)
{
	struct lsec_ctrl_peer *peer = &c->peer[slot];
	uint8_t body[GTK_BODY];

	lsec_group_body(&c->group, body);
	send_sealed(c, slot, KIND_JOIN_OK, body, sizeof(body));
	lsec_wipe(body, sizeof(body));

	peer->ok_sent = lsec_jittered(now, LSEC_RESEND_MS);
	peer->ok_tries++;
}

static void join_rsp(struct lsec_ctrl *c, uint8_t slot, const uint8_t *buf, size_t len,
		     int64_t now)
{
	struct lsec_ctrl_peer *peer = &c->peer[slot];
	const uint8_t *ns = buf + LINK_HDR;
	uint8_t expect[TAG_BYTES];
	uint8_t c2s[LSEC_KEY_BYTES];
	uint8_t s2c[LSEC_KEY_BYTES];

	if (len != LINK_HDR + NONCE_JOIN + TAG_BYTES) {
		c->stats.malformed++;
		return;
	}
	if (!peer->hs.pending) {
		c->stats.replay++;
		return;
	}

	if (lsec_join_mac(&peer->pairing, peer->hs.nc, ns, expect) ||
	    !lsec_equal(expect, ns + NONCE_JOIN, TAG_BYTES)) {
		c->stats.bad_tag++;
		return;
	}

	if (lsec_join_keys(&peer->pairing, peer->hs.nc, ns, c2s, s2c)) {
		return;
	}

	/* The satellite is live and knows the key: start the new session. */
	memset(&peer->tx, 0, sizeof(peer->tx));
	memset(&peer->rx, 0, sizeof(peer->rx));
	memcpy(peer->tx.key, c2s, LSEC_KEY_BYTES);
	memcpy(peer->rx.key, s2c, LSEC_KEY_BYTES);
	lsec_wipe(c2s, sizeof(c2s));
	lsec_wipe(s2c, sizeof(s2c));

	peer->hs.pending = false;
	peer->joined = false;
	peer->ok_pending = true;
	peer->gtk_pending = false;
	peer->ok_tries = 0;
	c->stats.rx_ok++;

	send_join_ok(c, slot, now);
}

static void sealed_rx(struct lsec_ctrl *c, const struct lsec_hdr *h, const uint8_t *buf,
		      size_t len, int64_t now)
{
	struct lsec_ctrl_peer *peer = &c->peer[h->src];
	uint8_t body[LSEC_MAX_PAYLOAD];
	int n;

	if (!peer->joined && !peer->ok_pending) {
		c->stats.no_session++;
		return;
	}

	n = lsec_open(&peer->rx, c->net_id, h, buf, len, body);
	if (n == -EALREADY) {
		c->stats.replay++;
		return;
	}
	if (n < 0) {
		c->stats.bad_tag++;
		return;
	}

	peer->last_rx = now;
	c->stats.rx_ok++;

	switch (h->kind) {
	case KIND_JOIN_ACK:
		if (peer->ok_pending) {
			peer->ok_pending = false;
			peer->joined = true;

			struct lsec_event ev = {.type = LSEC_EV_JOINED, .slot = h->src};

			lsec_emit(&c->io, &ev);
		}
		break;

	case KIND_GROUP_ACK:
		if (n == 1 && body[0] == c->group.kid) {
			peer->gtk_pending = false;
		}
		break;

	case KIND_DATA:
		if (peer->joined) {
			struct lsec_event ev = {
				.type = LSEC_EV_DATA,
				.slot = h->src,
				.data = body,
				.len = n,
			};

			lsec_emit(&c->io, &ev);
		}
		break;

	default:
		c->stats.malformed++;
		break;
	}
}

void lsec_ctrl_rx(struct lsec_ctrl *c, const uint8_t *buf, size_t len, int16_t rssi_dbm,
		  int64_t now)
{
	int kind = lsec_frame_kind(buf, len);
	struct lsec_hdr h;

	if (kind < 0) {
		c->stats.malformed++;
		return;
	}

	if (KIND_IS_PAIR(kind)) {
		ctrl_pair_rx(c, kind, buf, len, rssi_dbm, now);
		return;
	}

	if (lsec_parse_hdr(buf, len, &h)) {
		c->stats.malformed++;
		return;
	}

	if (h.net_tag != (uint16_t)c->net_id || h.dst != LSEC_ADDR_CTRL || !valid_slot(h.src) ||
	    !c->peer[h.src].paired) {
		c->stats.foreign++;
		return;
	}

	switch (h.kind) {
	case KIND_HELLO:
		/*
		 * Unauthenticated, so all it may do is start a join. The
		 * running session stays until the satellite proves itself
		 * in JOIN_RSP.
		 */
		if (len == LINK_HDR &&
		    lsec_due(now, c->peer[h.src].hs.sent, LSEC_HELLO_JOIN_MS)) {
			join_start(c, h.src, now);
		}
		break;
	case KIND_JOIN_RSP:
		join_rsp(c, h.src, buf, len, now);
		break;
	case KIND_JOIN_ACK:
	case KIND_GROUP_ACK:
	case KIND_DATA:
		sealed_rx(c, &h, buf, len, now);
		break;
	default:
		c->stats.foreign++;
		break;
	}
}

int lsec_ctrl_send(struct lsec_ctrl *c, uint8_t dst, const void *data, size_t len)
{
	uint8_t out[LSEC_MAX_FRAME];
	struct lsec_dir *dir;
	uint8_t kid = 0;
	int n;

	if (dst == LSEC_ADDR_GROUP) {
		dir = &c->group.dir;
		kid = c->group.kid;
	} else if (valid_slot(dst) && c->peer[dst].joined) {
		dir = &c->peer[dst].tx;
	} else {
		return -ENOTCONN;
	}

	n = lsec_seal(dir, KIND_DATA, c->net_id, LSEC_ADDR_CTRL, dst, kid, data, len, out);
	if (n < 0) {
		return n;
	}

	tx(c, out, n);
	return 0;
}

int lsec_ctrl_send_poll(struct lsec_ctrl *c, uint8_t mask, uint8_t slot_ms, const void *data,
			size_t len)
{
	uint8_t body[LSEC_MAX_PAYLOAD];
	uint8_t out[LSEC_MAX_FRAME];
	int n;

	if (mask == 0 || slot_ms == 0) {
		return -EINVAL;
	}
	if (len > LSEC_MAX_POLL_PAYLOAD) {
		return -EMSGSIZE;
	}
	if (!c->group.valid) {
		return -ENOTCONN;
	}

	body[0] = mask;
	body[1] = slot_ms;
	memcpy(body + LSEC_POLL_HDR, data, len);

	n = lsec_seal(&c->group.dir, KIND_POLL, c->net_id, LSEC_ADDR_CTRL, LSEC_ADDR_GROUP,
		      c->group.kid, body, LSEC_POLL_HDR + len, out);
	if (n < 0) {
		return n;
	}

	tx(c, out, n);
	return 0;
}

void lsec_ctrl_poll(struct lsec_ctrl *c, int64_t now)
{
	struct lsec_pair *p = &c->pair;

	if (p->state != CP_IDLE) {
		if (lsec_due(now, p->started, LSEC_PAIR_TIMEOUT_MS)) {
			pair_abort(c, LSEC_FAIL_TIMEOUT, true);
			return;
		}
		/* Keep nudging until the satellite answers. CHK_C also waits
		 * for the satellite's user, so it repeats until the timeout.
		 */
		if (p->state != CP_CONFIRM && lsec_due(now, p->last_tx, LSEC_PAIR_RETRY_MS)) {
			pair_tx(c, p->last_frame, p->last_len, now);
		}
		/* The radio is on the pairing profile; the links wait. */
		return;
	}

	for (uint8_t slot = 1; slot <= LSEC_MAX_SATS; slot++) {
		struct lsec_ctrl_peer *peer = &c->peer[slot];

		if (!peer->paired) {
			continue;
		}

		/* Nothing from it for too long: it is gone, or cannot hear us. */
		if (peer->joined && c->peer_timeout_ms &&
		    lsec_due(now, peer->last_rx, c->peer_timeout_ms)) {
			struct lsec_event ev = {.type = LSEC_EV_LEFT, .slot = slot};

			peer_drop_session(peer);
			lsec_emit(&c->io, &ev);
		}

		if (peer->ok_pending && lsec_due(now, peer->ok_sent, LSEC_RESEND_MS)) {
			if (peer->ok_tries >= OK_TRIES) {
				peer->ok_pending = false;
			} else {
				send_join_ok(c, slot, now);
			}
		}

		if (!peer->joined && !peer->ok_pending &&
		    lsec_due(now, peer->join_retry_from, LSEC_JOIN_RETRY_MS)) {
			join_start(c, slot, now);
		}

		if (peer->joined && peer->gtk_pending &&
		    lsec_due(now, peer->gtk_sent, LSEC_RESEND_MS)) {
			send_group_update(c, slot, now);
		}
	}
}
