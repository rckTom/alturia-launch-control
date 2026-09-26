/* SPDX-License-Identifier: Apache-2.0 */

/* The PSA calls behind lsec. Keys are imported per operation and destroyed
 * straight after, so the number of lsec keys never presses on the PSA key
 * slot count.
 */

#include <errno.h>
#include <string.h>
#include <psa/crypto.h>

#include "lsec_internal.h"

#define CCM_ALG PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, TAG_BYTES)
#define CCM_NONCE 13

int lsec_crypto_init(void)
{
	return psa_crypto_init() == PSA_SUCCESS ? 0 : -EIO;
}

int lsec_random(void *buf, size_t len)
{
	return psa_generate_random(buf, len) == PSA_SUCCESS ? 0 : -EIO;
}

int64_t lsec_jittered(int64_t now, uint32_t interval)
{
	uint32_t r;

	if (interval < 4 || lsec_random(&r, sizeof(r))) {
		return now;
	}
	return now + (int64_t)(r % (interval / 2 + 1)) - interval / 4;
}

bool lsec_equal(const void *a, const void *b, size_t len)
{
	const volatile uint8_t *x = a;
	const volatile uint8_t *y = b;
	uint8_t diff = 0;

	for (size_t i = 0; i < len; i++) {
		diff |= x[i] ^ y[i];
	}

	return diff == 0;
}

void lsec_wipe(void *p, size_t len)
{
	volatile uint8_t *b = p;

	while (len--) {
		*b++ = 0;
	}
}

int lsec_hmac(const uint8_t *key, size_t key_len, const struct lsec_part *parts, size_t n,
	      uint8_t out[32])
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
	psa_key_id_t id;
	psa_status_t st;
	size_t len;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
	psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
	psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
	psa_set_key_bits(&attr, key_len * 8);

	if (psa_import_key(&attr, key, key_len, &id) != PSA_SUCCESS) {
		return -EIO;
	}

	st = psa_mac_sign_setup(&op, id, PSA_ALG_HMAC(PSA_ALG_SHA_256));
	for (size_t i = 0; i < n && st == PSA_SUCCESS; i++) {
		st = psa_mac_update(&op, parts[i].p, parts[i].len);
	}
	if (st == PSA_SUCCESS) {
		st = psa_mac_sign_finish(&op, out, 32, &len);
	}

	psa_mac_abort(&op);
	psa_destroy_key(id);

	return st == PSA_SUCCESS ? 0 : -EIO;
}

int lsec_hkdf(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
	      const struct lsec_part *info, size_t n, uint8_t *out, size_t out_len)
{
	psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
	uint8_t buf[128];
	size_t info_len = 0;
	psa_status_t st;

	/* PSA takes each input once, so the info parts are joined first. */
	for (size_t i = 0; i < n; i++) {
		if (info_len + info[i].len > sizeof(buf)) {
			return -EINVAL;
		}
		memcpy(buf + info_len, info[i].p, info[i].len);
		info_len += info[i].len;
	}

	st = psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256));
	if (st == PSA_SUCCESS && salt_len > 0) {
		st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt,
						    salt_len);
	}
	if (st == PSA_SUCCESS) {
		st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SECRET, ikm,
						    ikm_len);
	}
	if (st == PSA_SUCCESS) {
		st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO, buf,
						    info_len);
	}
	if (st == PSA_SUCCESS) {
		st = psa_key_derivation_output_bytes(&op, out, out_len);
	}

	psa_key_derivation_abort(&op);

	return st == PSA_SUCCESS ? 0 : -EIO;
}

int lsec_ecdh_generate(uint32_t *key_id, uint8_t pub[PK_BYTES])
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t id;
	size_t len;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);

	if (psa_generate_key(&attr, &id) != PSA_SUCCESS) {
		return -EIO;
	}

	if (psa_export_public_key(id, pub, PK_BYTES, &len) != PSA_SUCCESS || len != PK_BYTES) {
		psa_destroy_key(id);
		return -EIO;
	}

	*key_id = id;
	return 0;
}

int lsec_ecdh_agree(uint32_t key_id, const uint8_t peer[PK_BYTES], uint8_t secret[32])
{
	size_t len;

	/* PSA rejects points that are not on the curve. */
	if (psa_raw_key_agreement(PSA_ALG_ECDH, key_id, peer, PK_BYTES, secret, 32, &len) !=
		    PSA_SUCCESS ||
	    len != 32) {
		return -EIO;
	}

	return 0;
}

void lsec_ecdh_destroy(uint32_t *key_id)
{
	if (*key_id != PSA_KEY_ID_NULL) {
		psa_destroy_key(*key_id);
		*key_id = PSA_KEY_ID_NULL;
	}
}

static int aes_import(const uint8_t key[LSEC_KEY_BYTES], psa_key_id_t *id)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
	psa_set_key_algorithm(&attr, CCM_ALG);
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, LSEC_KEY_BYTES * 8);

	return psa_import_key(&attr, key, LSEC_KEY_BYTES, id) == PSA_SUCCESS ? 0 : -EIO;
}

int lsec_aead_seal(const uint8_t key[LSEC_KEY_BYTES], const uint8_t nonce[13],
		   const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t pt_len,
		   uint8_t *out)
{
	psa_key_id_t id;
	psa_status_t st;
	size_t len;

	if (aes_import(key, &id)) {
		return -EIO;
	}

	st = psa_aead_encrypt(id, CCM_ALG, nonce, CCM_NONCE, aad, aad_len, pt, pt_len, out,
			      pt_len + TAG_BYTES, &len);
	psa_destroy_key(id);

	return st == PSA_SUCCESS ? (int)len : -EIO;
}

int lsec_aead_open(const uint8_t key[LSEC_KEY_BYTES], const uint8_t nonce[13],
		   const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len,
		   uint8_t *out)
{
	psa_key_id_t id;
	psa_status_t st;
	size_t len;

	if (ct_len < TAG_BYTES) {
		return -EACCES;
	}

	if (aes_import(key, &id)) {
		return -EIO;
	}

	st = psa_aead_decrypt(id, CCM_ALG, nonce, CCM_NONCE, aad, aad_len, ct, ct_len, out,
			      ct_len - TAG_BYTES, &len);
	psa_destroy_key(id);

	if (st == PSA_ERROR_INVALID_SIGNATURE) {
		return -EACCES;
	}

	return st == PSA_SUCCESS ? (int)len : -EIO;
}
