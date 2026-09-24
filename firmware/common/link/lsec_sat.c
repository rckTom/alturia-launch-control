/* SPDX-License-Identifier: Apache-2.0 */

/* Satellite role: pairing responder, join responder, frame reception. */

#include <errno.h>
#include <string.h>
#include <psa/crypto.h>

#include "lsec_internal.h"

enum sat_pair_state {
	SP_IDLE,
	SP_ADV,        /* pairing mode, advertising */
	SP_COMMITTED,  /* PAIR_RSP sent */
	SP_CONFIRM,    /* PAIR_NS sent, SAS shown, waiting for the user and Ec */
	SP_DONE,       /* paired; answering late CHK_C for a moment */
};

static void tx(struct lsec_sat *s, const uint8_t *buf, size_t len)
{
	s->io.send(s->io.user, buf, len);
}

static void pair_tx(struct lsec_sat *s, const uint8_t *buf, size_t len)
{
	memcpy(s->pair.last_frame, buf, len);
	s->pair.last_len = len;
	tx(s, buf, len);
}

static void drop_session(struct lsec_sat *s)
{
	s->joined = false;
	lsec_wipe(&s->tx, sizeof(s->tx));
	lsec_wipe(&s->rx, sizeof(s->rx));
	lsec_wipe(&s->group, sizeof(s->group));
	lsec_wipe(&s->hs, sizeof(s->hs));
	lsec_wipe(s->hs_c2s, sizeof(s->hs_c2s));
	lsec_wipe(s->hs_s2c, sizeof(s->hs_s2c));
	s->last_hello = LSEC_NEVER;
	s->last_rx = LSEC_NEVER;
}

int lsec_sat_init(struct lsec_sat *s, const struct lsec_io *io, uint64_t uid)
{
	memset(s, 0, sizeof(*s));
	s->io = *io;
	s->uid = uid;
	s->pair.key_id = PSA_KEY_ID_NULL;
	drop_session(s);

	return lsec_crypto_init();
}

int lsec_sat_load(struct lsec_sat *s, const struct lsec_pairing *p)
{
	if (p->slot == 0 || p->slot > LSEC_MAX_SATS) {
		return -EINVAL;
	}

	s->pairing = *p;
	s->paired = true;
	drop_session(s);

	return 0;
}

int lsec_sat_unpair(struct lsec_sat *s)
{
	int ret = s->io.store ? s->io.store(s->io.user, 0, NULL) : 0;

	lsec_wipe(&s->pairing, sizeof(s->pairing));
	s->paired = false;
	drop_session(s);

	return ret;
}

bool lsec_sat_joined(const struct lsec_sat *s)
{
	return s->joined;
}

bool lsec_sat_paired(const struct lsec_sat *s)
{
	return s->paired;
}

bool lsec_sat_pairing_active(const struct lsec_sat *s)
{
	return s->pair.state != SP_IDLE;
}

/* ---- pairing ---------------------------------------------------------- */

static void pair_abort(struct lsec_sat *s, enum lsec_fail reason, bool tell_peer)
{
	uint64_t uid = s->uid;

	if (tell_peer) {
		uint8_t buf[PAIR_HDR + 1];

		lsec_pair_hdr(buf, KIND_PAIR_ABORT, s->uid);
		buf[PAIR_HDR] = reason;
		tx(s, buf, sizeof(buf));
	}

	/* A failed attempt ends pairing mode; a new one needs the gesture. */
	lsec_pair_reset(&s->pair);
	lsec_emit_fail(&s->io, uid, reason);
}

int lsec_sat_pair_start(struct lsec_sat *s, int64_t now)
{
	uint8_t buf[PAIR_HDR];

	lsec_pair_reset(&s->pair);
	s->pair.state = SP_ADV;
	s->pair.uid = s->uid;
	s->pair.started = now;
	s->pair.last_adv = now;

	lsec_pair_hdr(buf, KIND_PAIR_ADV, s->uid);
	tx(s, buf, sizeof(buf));

	return 0;
}

static void pair_finish(struct lsec_sat *s, int64_t now)
{
	struct lsec_pair *p = &s->pair;
	struct lsec_pairing rec = {
		.sat_uid = s->uid,
		.net_id = p->net_id,
		.slot = p->slot,
	};
	uint8_t buf[PAIR_HDR + CHECK_BYTES];
	int ret;

	if (!p->user_ok || !p->check_ok) {
		return;
	}

	memcpy(rec.ltk, p->ltk, sizeof(rec.ltk));

	ret = s->io.store ? s->io.store(s->io.user, 0, &rec) : 0;
	if (ret) {
		lsec_wipe(&rec, sizeof(rec));
		pair_abort(s, LSEC_FAIL_STORE, true);
		return;
	}

	if (lsec_pair_check(p, false, false, &buf[PAIR_HDR])) {
		lsec_wipe(&rec, sizeof(rec));
		pair_abort(s, LSEC_FAIL_CRYPTO, true);
		return;
	}

	/* A new pairing replaces the old one and its session. */
	s->pairing = rec;
	s->paired = true;
	drop_session(s);
	lsec_wipe(&rec, sizeof(rec));

	lsec_pair_hdr(buf, KIND_PAIR_CHK_S, s->uid);
	pair_tx(s, buf, sizeof(buf));

	p->state = SP_DONE;
	p->last_tx = now;

	struct lsec_event ev = {
		.type = LSEC_EV_PAIRED,
		.slot = s->pairing.slot,
		.uid = s->uid,
	};

	lsec_emit(&s->io, &ev);
}

void lsec_sat_pair_confirm(struct lsec_sat *s, bool accept, int64_t now)
{
	if (s->pair.state == SP_IDLE || s->pair.state == SP_DONE) {
		return;
	}

	if (!accept) {
		pair_abort(s, LSEC_FAIL_REJECTED, s->pair.state >= SP_COMMITTED);
		return;
	}

	if (s->pair.state == SP_CONFIRM) {
		s->pair.user_ok = true;
		pair_finish(s, now);
	}
}

static void sat_pair_rx(struct lsec_sat *s, int kind, const uint8_t *buf, size_t len,
			int64_t now)
{
	struct lsec_pair *p = &s->pair;
	const uint8_t *body = buf + PAIR_HDR;
	size_t body_len = len - PAIR_HDR;
	uint8_t out[PAIR_HDR + PK_BYTES + CHECK_BYTES];

	if (len < PAIR_HDR || get_le64(&buf[1]) != s->uid || p->state == SP_IDLE) {
		s->stats.foreign++;
		return;
	}

	switch (kind) {
	case KIND_PAIR_REQ:
		if (body_len != 4 + PK_BYTES) {
			break;
		}
		if (p->state == SP_COMMITTED && memcmp(p->pk_peer, body + 4, PK_BYTES) == 0) {
			tx(s, p->last_frame, p->last_len);   /* our PAIR_RSP was lost */
			return;
		}
		if (p->state != SP_ADV) {
			return;   /* committed to another controller's key */
		}

		p->net_id = get_le32(body);
		memcpy(p->pk_peer, body + 4, PK_BYTES);

		if (lsec_ecdh_generate(&p->key_id, p->pk_own) ||
		    lsec_random(p->n_own, NONCE_PAIR) ||
		    lsec_pair_commitment(p->n_own, p->pk_own, p->pk_peer, p->commit)) {
			pair_abort(s, LSEC_FAIL_CRYPTO, true);
			return;
		}

		lsec_pair_hdr(out, KIND_PAIR_RSP, s->uid);
		memcpy(out + PAIR_HDR, p->pk_own, PK_BYTES);
		memcpy(out + PAIR_HDR + PK_BYTES, p->commit, CHECK_BYTES);
		pair_tx(s, out, PAIR_HDR + PK_BYTES + CHECK_BYTES);
		p->state = SP_COMMITTED;
		s->stats.rx_ok++;
		return;

	case KIND_PAIR_NC: {
		struct lsec_event ev = {.type = LSEC_EV_SAS, .uid = s->uid};

		if (body_len != NONCE_PAIR) {
			break;
		}
		if (p->state == SP_CONFIRM && memcmp(p->n_peer, body, NONCE_PAIR) == 0) {
			tx(s, p->last_frame, p->last_len);   /* our PAIR_NS was lost */
			return;
		}
		if (p->state != SP_COMMITTED) {
			return;
		}

		memcpy(p->n_peer, body, NONCE_PAIR);

		if (lsec_pair_derive(p, false, ev.sas)) {
			pair_abort(s, LSEC_FAIL_CRYPTO, true);
			return;
		}

		lsec_pair_hdr(out, KIND_PAIR_NS, s->uid);
		memcpy(out + PAIR_HDR, p->n_own, NONCE_PAIR);
		pair_tx(s, out, PAIR_HDR + NONCE_PAIR);
		p->state = SP_CONFIRM;
		s->stats.rx_ok++;
		lsec_emit(&s->io, &ev);
		return;
	}

	case KIND_PAIR_CHK_C: {
		uint8_t expect[CHECK_BYTES];

		if (body_len != 1 + CHECK_BYTES) {
			break;
		}
		if (p->state == SP_DONE) {
			tx(s, p->last_frame, p->last_len);   /* our PAIR_CHK_S was lost */
			return;
		}
		if (p->state != SP_CONFIRM || p->check_ok) {
			return;
		}
		if (body[0] == 0 || body[0] > LSEC_MAX_SATS) {
			break;
		}

		/* The slot is covered by Ec, so it cannot be altered on the way. */
		p->slot = body[0];
		if (lsec_pair_check(p, false, true, expect)) {
			pair_abort(s, LSEC_FAIL_CRYPTO, true);
			return;
		}
		if (!lsec_equal(expect, body + 1, CHECK_BYTES)) {
			pair_abort(s, LSEC_FAIL_CHECK, true);
			return;
		}

		p->check_ok = true;
		s->stats.rx_ok++;
		pair_finish(s, now);
		return;
	}

	case KIND_PAIR_ABORT:
		if (p->state >= SP_COMMITTED && p->state != SP_DONE) {
			pair_abort(s, LSEC_FAIL_PEER_ABORT, false);
		}
		return;

	default:
		return;   /* ADV, RSP, NS, CHK_S are the satellite's own */
	}

	s->stats.malformed++;
}

/* ---- session ---------------------------------------------------------- */

static void join_req(struct lsec_sat *s, const struct lsec_hdr *h, const uint8_t *buf,
		     size_t len)
{
	uint8_t out[LINK_HDR + NONCE_JOIN + TAG_BYTES];

	if (len != LINK_HDR + NONCE_JOIN || h->dst != s->pairing.slot) {
		s->stats.malformed++;
		return;
	}

	/*
	 * Anyone can send a JOIN_REQ, so it must not disturb the running
	 * session. The new keys only take over when a JOIN_OK sealed with
	 * them proves the request came from the controller.
	 */
	memcpy(s->hs.nc, buf + LINK_HDR, NONCE_JOIN);
	if (lsec_random(s->hs.ns, NONCE_JOIN) ||
	    lsec_join_keys(&s->pairing, s->hs.nc, s->hs.ns, s->hs_c2s, s->hs_s2c)) {
		return;
	}

	lsec_link_hdr(out, KIND_JOIN_RSP, s->pairing.net_id, s->pairing.slot, LSEC_ADDR_CTRL);
	memcpy(out + LINK_HDR, s->hs.ns, NONCE_JOIN);
	if (lsec_join_mac(&s->pairing, s->hs.nc, s->hs.ns, out + LINK_HDR + NONCE_JOIN)) {
		return;
	}

	s->hs.pending = true;
	tx(s, out, sizeof(out));
}

static void send_sealed(struct lsec_sat *s, enum lsec_kind kind, const void *data, size_t len)
{
	uint8_t out[LSEC_MAX_FRAME];
	int n;

	n = lsec_seal(&s->tx, kind, s->pairing.net_id, s->pairing.slot, LSEC_ADDR_CTRL, 0, data,
		      len, out);
	if (n > 0) {
		tx(s, out, n);
	}
}

static void join_ok(struct lsec_sat *s, const struct lsec_hdr *h, const uint8_t *buf,
		    size_t len, int64_t now)
{
	uint8_t body[LSEC_MAX_PAYLOAD];
	int n = -EACCES;

	if (h->dst != s->pairing.slot) {
		s->stats.malformed++;
		return;
	}

	if (s->hs.pending) {
		struct lsec_dir rx = {.seen = false};

		memcpy(rx.key, s->hs_c2s, LSEC_KEY_BYTES);
		n = lsec_open(&rx, s->pairing.net_id, h, buf, len, body);
		if (n == GTK_BODY) {
			/* The controller proved itself: switch to the new session. */
			s->rx = rx;
			memset(&s->tx, 0, sizeof(s->tx));
			memcpy(s->tx.key, s->hs_s2c, LSEC_KEY_BYTES);
			memset(&s->group, 0, sizeof(s->group));
			lsec_group_install(&s->group, body);
			s->hs.pending = false;
			s->joined = true;
			s->last_rx = now;
			s->stats.rx_ok++;

			send_sealed(s, KIND_JOIN_ACK, NULL, 0);

			struct lsec_event ev = {.type = LSEC_EV_JOINED, .slot = s->pairing.slot};

			lsec_emit(&s->io, &ev);
			return;
		}
	}

	/* A resend from the current session: our JOIN_ACK was lost. */
	if (s->joined) {
		n = lsec_open(&s->rx, s->pairing.net_id, h, buf, len, body);
		if (n == GTK_BODY) {
			lsec_group_install(&s->group, body);
			s->last_rx = now;
			s->stats.rx_ok++;
			send_sealed(s, KIND_JOIN_ACK, NULL, 0);
			return;
		}
	}

	if (n == -EALREADY) {
		s->stats.replay++;
	} else {
		s->stats.bad_tag++;
	}
}

static void sealed_rx(struct lsec_sat *s, const struct lsec_hdr *h, const uint8_t *buf,
		      size_t len, int64_t now)
{
	uint8_t body[LSEC_MAX_PAYLOAD];
	bool group = h->dst == LSEC_ADDR_GROUP;
	struct lsec_dir *dir;
	int n;

	if (!s->joined) {
		s->stats.no_session++;
		return;
	}

	if (group) {
		/* Only application data is broadcast. */
		if (h->kind != KIND_DATA || !s->group.valid || h->kid != s->group.kid) {
			s->stats.bad_tag++;
			return;
		}
		dir = &s->group.dir;
	} else {
		dir = &s->rx;
	}

	n = lsec_open(dir, s->pairing.net_id, h, buf, len, body);
	if (n == -EALREADY) {
		s->stats.replay++;
		return;
	}
	if (n < 0) {
		s->stats.bad_tag++;
		return;
	}

	s->last_rx = now;
	s->stats.rx_ok++;

	if (h->kind == KIND_GROUP_UPDATE) {
		if (n == GTK_BODY) {
			lsec_group_install(&s->group, body);
			send_sealed(s, KIND_GROUP_ACK, &s->group.kid, 1);
		}
		return;
	}

	struct lsec_event ev = {
		.type = LSEC_EV_DATA,
		.slot = s->pairing.slot,
		.group = group,
		.data = body,
		.len = n,
	};

	lsec_emit(&s->io, &ev);
}

void lsec_sat_rx(struct lsec_sat *s, const uint8_t *buf, size_t len, int64_t now)
{
	int kind = lsec_frame_kind(buf, len);
	struct lsec_hdr h;

	if (kind < 0) {
		s->stats.malformed++;
		return;
	}

	if (KIND_IS_PAIR(kind)) {
		sat_pair_rx(s, kind, buf, len, now);
		return;
	}

	if (lsec_parse_hdr(buf, len, &h)) {
		s->stats.malformed++;
		return;
	}

	if (!s->paired || h.net_tag != (uint16_t)s->pairing.net_id || h.src != LSEC_ADDR_CTRL ||
	    (h.dst != s->pairing.slot && h.dst != LSEC_ADDR_GROUP)) {
		s->stats.foreign++;
		return;
	}

	switch (h.kind) {
	case KIND_JOIN_REQ:
		join_req(s, &h, buf, len);
		break;
	case KIND_JOIN_OK:
		join_ok(s, &h, buf, len, now);
		break;
	case KIND_GROUP_UPDATE:
	case KIND_DATA:
		if (h.kind == KIND_GROUP_UPDATE && h.dst == LSEC_ADDR_GROUP) {
			s->stats.malformed++;
			break;
		}
		sealed_rx(s, &h, buf, len, now);
		break;
	default:
		s->stats.foreign++;
		break;
	}
}

int lsec_sat_send(struct lsec_sat *s, const void *data, size_t len)
{
	uint8_t out[LSEC_MAX_FRAME];
	int n;

	if (!s->joined) {
		return -ENOTCONN;
	}

	n = lsec_seal(&s->tx, KIND_DATA, s->pairing.net_id, s->pairing.slot, LSEC_ADDR_CTRL, 0,
		      data, len, out);
	if (n < 0) {
		return n;
	}

	tx(s, out, n);
	return 0;
}

void lsec_sat_poll(struct lsec_sat *s, int64_t now)
{
	struct lsec_pair *p = &s->pair;

	if (p->state == SP_DONE) {
		if (lsec_due(now, p->last_tx, LSEC_PAIR_LINGER_MS)) {
			lsec_pair_reset(p);
		}
		return;
	}

	if (p->state != SP_IDLE) {
		if (lsec_due(now, p->started, LSEC_PAIR_WINDOW_MS)) {
			pair_abort(s, LSEC_FAIL_TIMEOUT, p->state >= SP_COMMITTED);
			return;
		}
		if (p->state == SP_ADV && lsec_due(now, p->last_adv, LSEC_ADV_MS)) {
			uint8_t buf[PAIR_HDR];

			lsec_pair_hdr(buf, KIND_PAIR_ADV, s->uid);
			tx(s, buf, sizeof(buf));
			p->last_adv = now;
		}
		return;
	}

	/* No session, or the controller has gone quiet: ask for a join. */
	if (s->paired && (!s->joined || lsec_due(now, s->last_rx, LSEC_RX_IDLE_MS)) &&
	    lsec_due(now, s->last_hello, LSEC_HELLO_MS)) {
		uint8_t buf[LINK_HDR];

		lsec_link_hdr(buf, KIND_HELLO, s->pairing.net_id, s->pairing.slot,
			      LSEC_ADDR_CTRL);
		tx(s, buf, sizeof(buf));
		s->last_hello = now;
	}
}
