/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * Pairing maths shared by both roles: numeric comparison after Bluetooth LE
 * Secure Connections. The satellite commits to its nonce before it sees the
 * controller's, so a man in the middle cannot search for values that make
 * both screens show the same SAS; each attempt wins with probability 2^-24.
 */

#include <string.h>
#include <psa/crypto.h>

#include "lsec_internal.h"

#define X(pk) ((pk) + 1)   /* x coordinate of an uncompressed P-256 point */
#define X_BYTES 32

void lsec_pair_reset(struct lsec_pair *p)
{
	uint32_t id = p->key_id;

	lsec_ecdh_destroy(&id);
	lsec_wipe(p, sizeof(*p));
	p->key_id = PSA_KEY_ID_NULL;
}

void lsec_emit(const struct lsec_io *io, const struct lsec_event *ev)
{
	if (io->event) {
		io->event(io->user, ev);
	}
}

void lsec_emit_fail(const struct lsec_io *io, uint64_t uid, enum lsec_fail reason)
{
	struct lsec_event ev = {
		.type = LSEC_EV_PAIR_FAILED,
		.uid = uid,
		.reason = reason,
	};

	lsec_emit(io, &ev);
}

size_t lsec_pair_hdr(uint8_t *buf, enum lsec_kind kind, uint64_t uid)
{
	buf[0] = lsec_kind_byte(kind);
	put_le64(&buf[1], uid);

	return PAIR_HDR;
}

int lsec_pair_commitment(const uint8_t ns[NONCE_PAIR], const uint8_t pks[PK_BYTES],
			 const uint8_t pkc[PK_BYTES], uint8_t out[CHECK_BYTES])
{
	const struct lsec_part parts[] = {{X(pks), X_BYTES}, {X(pkc), X_BYTES}};
	uint8_t mac[32];
	int ret;

	ret = lsec_hmac(ns, NONCE_PAIR, parts, 2, mac);
	memcpy(out, mac, CHECK_BYTES);

	return ret;
}

int lsec_pair_derive(struct lsec_pair *p, bool is_ctrl, uint8_t sas[LSEC_SAS_FRAMES])
{
	static const char label[] = "lc-pair-v1";
	const uint8_t *pkc = is_ctrl ? p->pk_own : p->pk_peer;
	const uint8_t *pks = is_ctrl ? p->pk_peer : p->pk_own;
	const uint8_t *nc = is_ctrl ? p->n_own : p->n_peer;
	const uint8_t *ns = is_ctrl ? p->n_peer : p->n_own;
	uint8_t dhkey[32];
	uint8_t mac[32];
	uint8_t salt[2 * NONCE_PAIR];
	uint8_t ids[12];
	uint8_t keys[32 + LSEC_KEY_BYTES];
	uint32_t v;
	int ret;

	const struct lsec_part sas_in[] = {{X(pkc), X_BYTES}, {X(pks), X_BYTES}, {ns, NONCE_PAIR}};

	ret = lsec_hmac(nc, NONCE_PAIR, sas_in, 3, mac);
	if (ret) {
		return ret;
	}

	/* 24 bits, most significant first, as four 6-LED frames. */
	v = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2];
	for (int i = 0; i < LSEC_SAS_FRAMES; i++) {
		sas[i] = (v >> (6 * (LSEC_SAS_FRAMES - 1 - i))) & 0x3F;
	}

	ret = lsec_ecdh_agree(p->key_id, p->pk_peer, dhkey);
	lsec_ecdh_destroy(&p->key_id);
	if (ret) {
		return ret;
	}

	memcpy(salt, nc, NONCE_PAIR);
	memcpy(salt + NONCE_PAIR, ns, NONCE_PAIR);
	put_le32(&ids[0], p->net_id);
	put_le64(&ids[4], p->uid);

	const struct lsec_part info[] = {{label, sizeof(label) - 1},
					 {ids, sizeof(ids)},
					 {X(pkc), X_BYTES},
					 {X(pks), X_BYTES}};

	ret = lsec_hkdf(salt, sizeof(salt), dhkey, sizeof(dhkey), info, 4, keys, sizeof(keys));
	if (ret == 0) {
		memcpy(p->mac_key, keys, sizeof(p->mac_key));
		memcpy(p->ltk, keys + sizeof(p->mac_key), sizeof(p->ltk));
	}

	lsec_wipe(dhkey, sizeof(dhkey));
	lsec_wipe(keys, sizeof(keys));

	return ret;
}

int lsec_pair_check(const struct lsec_pair *p, bool is_ctrl, bool from_ctrl,
		    uint8_t out[CHECK_BYTES])
{
	const uint8_t *nc = is_ctrl ? p->n_own : p->n_peer;
	const uint8_t *ns = is_ctrl ? p->n_peer : p->n_own;
	uint8_t ids[13];
	uint8_t mac[32];
	int ret;

	put_le32(&ids[0], p->net_id);
	put_le64(&ids[4], p->uid);
	ids[12] = p->slot;

	/* Ec = HMAC(mac_key, "C" Nc Ns ids), Es = HMAC(mac_key, "S" Ns Nc ids) */
	const struct lsec_part parts[] = {{from_ctrl ? "C" : "S", 1},
					  {from_ctrl ? nc : ns, NONCE_PAIR},
					  {from_ctrl ? ns : nc, NONCE_PAIR},
					  {ids, sizeof(ids)}};

	ret = lsec_hmac(p->mac_key, sizeof(p->mac_key), parts, 4, mac);
	memcpy(out, mac, CHECK_BYTES);

	return ret;
}
