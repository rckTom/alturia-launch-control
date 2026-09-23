/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <psa/crypto.h>
#include <string.h>

#include "satellite.h"

LOG_MODULE_REGISTER(keys, LOG_LEVEL_INF);

#define SESSION_KEY_BYTES 32
#define GCM_NONCE_BYTES   12
#define GCM_TAG_BYTES     16

static uint8_t session_key[SESSION_KEY_BYTES];
static psa_key_id_t session_id;
static bool have_key;

static int key_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "session") != 0 || len != sizeof(session_key)) {
		return -ENOENT;
	}

	if (read_cb(cb_arg, session_key, len) != (ssize_t)len) {
		return -EIO;
	}

	have_key = true;
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(sat_keys, "sat/key", NULL, key_set, NULL, NULL);

static int key_import(void)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
	psa_set_key_algorithm(&attr, PSA_ALG_GCM);
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, SESSION_KEY_BYTES * 8);

	if (session_id != PSA_KEY_ID_NULL) {
		psa_destroy_key(session_id);
		session_id = PSA_KEY_ID_NULL;
	}

	if (psa_import_key(&attr, session_key, sizeof(session_key), &session_id) != PSA_SUCCESS) {
		return -EIO;
	}

	return 0;
}

int keys_load(void)
{
	if (psa_crypto_init() != PSA_SUCCESS) {
		return -EIO;
	}

	settings_load_subtree("sat/key");

	if (!have_key) {
		return -ENOENT;
	}

	return key_import();
}

int keys_provision(const uint8_t *key, size_t len)
{
	int ret;

	if (len != sizeof(session_key)) {
		return -EINVAL;
	}

	memcpy(session_key, key, len);

	ret = settings_save_one("sat/key/session", session_key, sizeof(session_key));
	if (ret) {
		return ret;
	}

	have_key = true;
	LOG_INF("session key provisioned");

	return key_import();
}

int keys_seal(const uint8_t *plain, size_t len, uint8_t *out, size_t *out_len)
{
	uint8_t nonce[GCM_NONCE_BYTES];
	size_t body;

	if (!have_key) {
		return -ENOENT;
	}

	if (psa_generate_random(nonce, sizeof(nonce)) != PSA_SUCCESS) {
		return -EIO;
	}

	if (*out_len < sizeof(nonce) + len + GCM_TAG_BYTES) {
		return -ENOSPC;
	}

	memcpy(out, nonce, sizeof(nonce));

	if (psa_aead_encrypt(session_id, PSA_ALG_GCM, nonce, sizeof(nonce), NULL, 0, plain, len,
			     out + sizeof(nonce), *out_len - sizeof(nonce), &body) != PSA_SUCCESS) {
		return -EIO;
	}

	*out_len = sizeof(nonce) + body;
	return 0;
}

int keys_unseal(const uint8_t *sealed, size_t len, uint8_t *out, size_t *out_len)
{
	size_t body;

	if (!have_key) {
		return -ENOENT;
	}

	if (len <= GCM_NONCE_BYTES + GCM_TAG_BYTES) {
		return -EINVAL;
	}

	if (psa_aead_decrypt(session_id, PSA_ALG_GCM, sealed, GCM_NONCE_BYTES, NULL, 0,
			     sealed + GCM_NONCE_BYTES, len - GCM_NONCE_BYTES, out, *out_len,
			     &body) != PSA_SUCCESS) {
		return -EACCES;
	}

	*out_len = body;
	return 0;
}
