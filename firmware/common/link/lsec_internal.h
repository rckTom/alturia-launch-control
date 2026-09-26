/* SPDX-License-Identifier: Apache-2.0 */

/* Wire format and crypto helpers shared by the lsec_*.c files. */

#ifndef LSEC_INTERNAL_H_
#define LSEC_INTERNAL_H_

#include "lsec.h"

#define LSEC_VERSION 1

/* Frame kinds, the low 6 bits of byte 0. The top two bits are the version. */
enum lsec_kind {
	/* Pairing: [kind][sat_uid 8][body] */
	KIND_PAIR_ADV = 1,
	KIND_PAIR_REQ,       /* net_id 4, PKc 65 */
	KIND_PAIR_RSP,       /* PKs 65, commitment 16 */
	KIND_PAIR_NC,        /* Nc 16 */
	KIND_PAIR_NS,        /* Ns 16 */
	KIND_PAIR_CHK_C,     /* slot 1, Ec 16 */
	KIND_PAIR_CHK_S,     /* Es 16 */
	KIND_PAIR_ABORT,     /* reason 1 */

	/* Join, in the clear: [kind][net_tag 2][src][dst][body] */
	KIND_HELLO = 16,     /* no body */
	KIND_JOIN_REQ,       /* Nc 8 */
	KIND_JOIN_RSP,       /* Ns 8, mac 8 */

	/* Sealed: [kind][net_tag 2][src][dst][kid][ctr 4][ciphertext][tag 8] */
	KIND_JOIN_OK = 32,   /* GTK 16, kid 1, group ctr 4 */
	KIND_JOIN_ACK,       /* empty */
	KIND_GROUP_UPDATE,   /* GTK 16, kid 1, group ctr 4 */
	KIND_GROUP_ACK,      /* kid 1 */
	KIND_DATA,           /* application payload */
	KIND_POLL,           /* group only: mask 1, slot_ms 1, application payload */
};

#define KIND_IS_PAIR(k)   ((k) >= KIND_PAIR_ADV && (k) <= KIND_PAIR_ABORT)
#define KIND_IS_SEALED(k) ((k) >= KIND_JOIN_OK && (k) <= KIND_POLL)

#define PAIR_HDR  9
#define LINK_HDR  5
#define SEAL_HDR  10
#define TAG_BYTES 8

#define PK_BYTES    65
#define NONCE_PAIR  16
#define NONCE_JOIN  8
#define CHECK_BYTES 16
#define GTK_BODY    (LSEC_KEY_BYTES + 1 + 4)

static inline uint8_t lsec_kind_byte(enum lsec_kind k)
{
	return (LSEC_VERSION << 6) | (uint8_t)k;
}

static inline void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = v;
	p[1] = v >> 8;
}

static inline void put_le32(uint8_t *p, uint32_t v)
{
	put_le16(p, v);
	put_le16(p + 2, v >> 16);
}

static inline void put_le64(uint8_t *p, uint64_t v)
{
	put_le32(p, v);
	put_le32(p + 4, v >> 32);
}

static inline uint16_t get_le16(const uint8_t *p)
{
	return p[0] | (p[1] << 8);
}

static inline uint32_t get_le32(const uint8_t *p)
{
	return get_le16(p) | ((uint32_t)get_le16(p + 2) << 16);
}

static inline uint64_t get_le64(const uint8_t *p)
{
	return get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

/* Returns the kind of a frame, or -1 if it is too short or another version. */
int lsec_frame_kind(const uint8_t *buf, size_t len);

/* Header of a join or sealed frame. */
struct lsec_hdr {
	enum lsec_kind kind;
	uint16_t net_tag;
	uint8_t src;
	uint8_t dst;
	uint8_t kid;   /* sealed only */
	uint32_t ctr;  /* sealed only */
};

size_t lsec_link_hdr(uint8_t *buf, enum lsec_kind kind, uint32_t net_id, uint8_t src,
		     uint8_t dst);
int lsec_parse_hdr(const uint8_t *buf, size_t len, struct lsec_hdr *h);

/*
 * Seal @p len bytes into @p out (LSEC_MAX_FRAME) under @p dir, using and
 * advancing its counter. Returns the frame length or a negative errno.
 */
int lsec_seal(struct lsec_dir *dir, enum lsec_kind kind, uint32_t net_id, uint8_t src,
	      uint8_t dst, uint8_t kid, const void *data, size_t len, uint8_t *out);

/*
 * Authenticate and decrypt a sealed frame whose header was parsed into @p h.
 * On success the replay window of @p dir moves and the plaintext is in
 * @p out. Returns its length, -EACCES on a bad tag or -EALREADY on a replay.
 * Nothing changes on failure.
 */
int lsec_open(struct lsec_dir *dir, uint32_t net_id, const struct lsec_hdr *h,
	      const uint8_t *buf, size_t len, uint8_t *out);

/* Crypto primitives, all through PSA. Return 0 or a negative errno. */
struct lsec_part {
	const void *p;
	size_t len;
};

int lsec_crypto_init(void);
int lsec_random(void *buf, size_t len);
int lsec_hmac(const uint8_t *key, size_t key_len, const struct lsec_part *parts, size_t n,
	      uint8_t out[32]);
int lsec_hkdf(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
	      const struct lsec_part *info, size_t n, uint8_t *out, size_t out_len);
int lsec_ecdh_generate(uint32_t *key_id, uint8_t pub[PK_BYTES]);
int lsec_ecdh_agree(uint32_t key_id, const uint8_t peer[PK_BYTES], uint8_t secret[32]);
void lsec_ecdh_destroy(uint32_t *key_id);
int lsec_aead_seal(const uint8_t key[LSEC_KEY_BYTES], const uint8_t nonce[13],
		   const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t pt_len,
		   uint8_t *out);
int lsec_aead_open(const uint8_t key[LSEC_KEY_BYTES], const uint8_t nonce[13],
		   const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len,
		   uint8_t *out);
/* Constant-time compare. */
bool lsec_equal(const void *a, const void *b, size_t len);
/* Wipe secrets so the compiler cannot drop it. */
void lsec_wipe(void *p, size_t len);

/* Pairing, shared by both roles. */
void lsec_pair_reset(struct lsec_pair *p);
void lsec_emit(const struct lsec_io *io, const struct lsec_event *ev);
void lsec_emit_fail(const struct lsec_io *io, uint64_t uid, enum lsec_fail reason);
/* Derive SAS, mac_key and LTK once both public keys and nonces are known. */
int lsec_pair_derive(struct lsec_pair *p, bool is_ctrl, uint8_t sas[LSEC_SAS_FRAMES]);
/* Ec (from_ctrl) or Es, computed from either side's view (is_ctrl). */
int lsec_pair_check(const struct lsec_pair *p, bool is_ctrl, bool from_ctrl,
		    uint8_t out[CHECK_BYTES]);
int lsec_pair_commitment(const uint8_t ns[NONCE_PAIR], const uint8_t pks[PK_BYTES],
			 const uint8_t pkc[PK_BYTES], uint8_t out[CHECK_BYTES]);
size_t lsec_pair_hdr(uint8_t *buf, enum lsec_kind kind, uint64_t uid);

/* Join: the satellite's proof, and the session keys both sides derive. */
int lsec_join_mac(const struct lsec_pairing *p, const uint8_t nc[NONCE_JOIN],
		  const uint8_t ns[NONCE_JOIN], uint8_t out[TAG_BYTES]);
int lsec_join_keys(const struct lsec_pairing *p, const uint8_t nc[NONCE_JOIN],
		   const uint8_t ns[NONCE_JOIN], uint8_t c2s[LSEC_KEY_BYTES],
		   uint8_t s2c[LSEC_KEY_BYTES]);

void lsec_group_body(const struct lsec_group *g, uint8_t out[GTK_BODY]);
/* Satellite side: take a group key from JOIN_OK or GROUP_UPDATE. */
void lsec_group_install(struct lsec_group *g, const uint8_t body[GTK_BODY]);

/* Far enough in the past that every interval has elapsed. */
#define LSEC_NEVER (INT64_MIN / 4)

static inline bool lsec_due(int64_t now, int64_t last, int64_t interval)
{
	return now - last >= interval;
}

/*
 * @p now moved by up to a quarter of @p interval either way, to record as
 * the time a repeating frame went out: the next one is then due after ¾
 * to 1¼ intervals. Retries at a fixed interval can stay in step with
 * another network's poll cycle and meet it every time; see
 * design/radio_security.md, "Other networks on the channel".
 */
int64_t lsec_jittered(int64_t now, uint32_t interval);

#endif /* LSEC_INTERNAL_H_ */
