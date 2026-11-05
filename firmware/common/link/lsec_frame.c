/* SPDX-License-Identifier: GPL-3.0-only */

#include <errno.h>
#include <string.h>

#include "lsec_internal.h"

int lsec_frame_kind(const uint8_t *buf, size_t len)
{
	if (len < 1 || (buf[0] >> 6) != LSEC_VERSION) {
		return -1;
	}

	return buf[0] & 0x3F;
}

size_t lsec_link_hdr(uint8_t *buf, enum lsec_kind kind, uint32_t net_id, uint8_t src,
		     uint8_t dst)
{
	buf[0] = lsec_kind_byte(kind);
	put_le16(&buf[1], (uint16_t)net_id);
	buf[3] = src;
	buf[4] = dst;

	return LINK_HDR;
}

int lsec_parse_hdr(const uint8_t *buf, size_t len, struct lsec_hdr *h)
{
	int kind = lsec_frame_kind(buf, len);

	if (kind < 0 || len < LINK_HDR) {
		return -EINVAL;
	}

	h->kind = kind;
	h->net_tag = get_le16(&buf[1]);
	h->src = buf[3];
	h->dst = buf[4];
	h->kid = 0;
	h->ctr = 0;

	if (KIND_IS_SEALED(kind)) {
		if (len < SEAL_HDR + TAG_BYTES) {
			return -EINVAL;
		}
		h->kid = buf[5];
		h->ctr = get_le32(&buf[6]);
	}

	return 0;
}

/*
 * The nonce never repeats under one key: each key belongs to one session
 * and one direction (or one group epoch), and ctr strictly increases.
 */
static void make_nonce(uint8_t nonce[13], uint32_t net_id, uint8_t src, uint8_t dst,
		       uint8_t kid, uint32_t ctr)
{
	put_le32(&nonce[0], net_id);
	nonce[4] = src;
	nonce[5] = dst;
	nonce[6] = kid;
	put_le32(&nonce[7], ctr);
	nonce[11] = 0;
	nonce[12] = 0;
}

int lsec_seal(struct lsec_dir *dir, enum lsec_kind kind, uint32_t net_id, uint8_t src,
	      uint8_t dst, uint8_t kid, const void *data, size_t len, uint8_t *out)
{
	uint8_t nonce[13];
	int ret;

	if (len > LSEC_MAX_PAYLOAD) {
		return -EMSGSIZE;
	}

	/* Never wrap: the session has to be re-joined long before. */
	if (dir->ctr == UINT32_MAX) {
		return -ENOSPC;
	}

	lsec_link_hdr(out, kind, net_id, src, dst);
	out[5] = kid;
	put_le32(&out[6], dir->ctr);

	make_nonce(nonce, net_id, src, dst, kid, dir->ctr);

	ret = lsec_aead_seal(dir->key, nonce, out, SEAL_HDR, data, len, out + SEAL_HDR);
	if (ret < 0) {
		return ret;
	}

	dir->ctr++;

	return SEAL_HDR + ret;
}

int lsec_open(struct lsec_dir *dir, uint32_t net_id, const struct lsec_hdr *h,
	      const uint8_t *buf, size_t len, uint8_t *out)
{
	uint8_t nonce[13];
	int ret;

	if (dir->seen && h->ctr <= dir->ctr) {
		return -EALREADY;
	}

	make_nonce(nonce, net_id, h->src, h->dst, h->kid, h->ctr);

	/* The whole header, kind included, is associated data. */
	ret = lsec_aead_open(dir->key, nonce, buf, SEAL_HDR, buf + SEAL_HDR, len - SEAL_HDR,
			     out);
	if (ret < 0) {
		return ret;
	}

	dir->ctr = h->ctr;
	dir->seen = true;

	return ret;
}

int lsec_join_mac(const struct lsec_pairing *p, const uint8_t nc[NONCE_JOIN],
		  const uint8_t ns[NONCE_JOIN], uint8_t out[TAG_BYTES])
{
	static const char label[] = "lc-auth-v1";
	uint8_t auth[32];
	uint8_t ids[13];
	uint8_t mac[32];
	int ret;

	put_le32(&ids[0], p->net_id);
	put_le64(&ids[4], p->sat_uid);
	ids[12] = p->slot;

	const struct lsec_part info[] = {{label, sizeof(label) - 1}};
	const struct lsec_part parts[] = {{"J", 1}, {nc, NONCE_JOIN}, {ns, NONCE_JOIN},
					  {ids, sizeof(ids)}};

	ret = lsec_hkdf(NULL, 0, p->ltk, sizeof(p->ltk), info, 1, auth, sizeof(auth));
	if (ret == 0) {
		ret = lsec_hmac(auth, sizeof(auth), parts, 4, mac);
	}

	memcpy(out, mac, TAG_BYTES);
	lsec_wipe(auth, sizeof(auth));
	lsec_wipe(mac, sizeof(mac));

	return ret;
}

int lsec_join_keys(const struct lsec_pairing *p, const uint8_t nc[NONCE_JOIN],
		   const uint8_t ns[NONCE_JOIN], uint8_t c2s[LSEC_KEY_BYTES],
		   uint8_t s2c[LSEC_KEY_BYTES])
{
	static const char label[] = "lc-sess-v1";
	uint8_t salt[2 * NONCE_JOIN];
	uint8_t ids[5];
	uint8_t keys[2 * LSEC_KEY_BYTES];
	int ret;

	memcpy(salt, nc, NONCE_JOIN);
	memcpy(salt + NONCE_JOIN, ns, NONCE_JOIN);
	put_le32(&ids[0], p->net_id);
	ids[4] = p->slot;

	const struct lsec_part info[] = {{label, sizeof(label) - 1}, {ids, sizeof(ids)}};

	ret = lsec_hkdf(salt, sizeof(salt), p->ltk, sizeof(p->ltk), info, 2, keys, sizeof(keys));
	if (ret == 0) {
		memcpy(c2s, keys, LSEC_KEY_BYTES);
		memcpy(s2c, keys + LSEC_KEY_BYTES, LSEC_KEY_BYTES);
	}

	lsec_wipe(keys, sizeof(keys));

	return ret;
}

void lsec_group_body(const struct lsec_group *g, uint8_t out[GTK_BODY])
{
	memcpy(out, g->dir.key, LSEC_KEY_BYTES);
	out[LSEC_KEY_BYTES] = g->kid;
	/* The next counter the controller will use; older frames are replays. */
	put_le32(&out[LSEC_KEY_BYTES + 1], g->dir.ctr);
}

void lsec_group_install(struct lsec_group *g, const uint8_t body[GTK_BODY])
{
	uint8_t kid = body[LSEC_KEY_BYTES];
	uint32_t next = get_le32(&body[LSEC_KEY_BYTES + 1]);
	bool same = g->valid && g->kid == kid && lsec_equal(g->dir.key, body, LSEC_KEY_BYTES);

	/*
	 * A resent JOIN_OK or GROUP_UPDATE carries the same key again. Never
	 * move the replay window back for it.
	 */
	if (same && g->dir.seen && next <= g->dir.ctr + 1) {
		return;
	}

	memcpy(g->dir.key, body, LSEC_KEY_BYTES);
	g->kid = kid;
	g->dir.seen = next > 0;
	g->dir.ctr = next > 0 ? next - 1 : 0;
	g->valid = true;
}
