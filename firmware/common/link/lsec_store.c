/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Pairings and the controller's net_id in Zephyr settings:
 *
 *   lsec/net      controller network id, u32
 *   lsec/p/<n>    pairing record; the controller keeps one per slot n,
 *                 the satellite its single one under n = 0
 *
 * Records are stored in the clear. Readout protection of the flash (RDP on
 * the STM32, flash encryption on the ESP32) is what keeps them there.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/settings/settings.h>

#include "lsec_internal.h"
#include "lsec_store.h"

#define REC_BYTES (8 + 4 + 1 + LSEC_KEY_BYTES)

static void encode(const struct lsec_pairing *p, uint8_t out[REC_BYTES])
{
	put_le64(&out[0], p->sat_uid);
	put_le32(&out[8], p->net_id);
	out[12] = p->slot;
	memcpy(&out[13], p->ltk, LSEC_KEY_BYTES);
}

static void decode(const uint8_t in[REC_BYTES], struct lsec_pairing *p)
{
	p->sat_uid = get_le64(&in[0]);
	p->net_id = get_le32(&in[8]);
	p->slot = in[12];
	memcpy(p->ltk, &in[13], LSEC_KEY_BYTES);
}

struct load_ctx {
	lsec_store_cb cb;
	void *user;
	uint32_t *net_id;
	bool have_net;
	int count;
};

static int load_one(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
		    void *param)
{
	struct load_ctx *ctx = param;
	const char *next;

	if (settings_name_steq(key, "net", NULL) && len == sizeof(uint32_t)) {
		if (ctx->net_id && read_cb(cb_arg, ctx->net_id, len) == (ssize_t)len) {
			ctx->have_net = true;
		}
		return 0;
	}

	if (settings_name_steq(key, "p", &next) && next && len == REC_BYTES && ctx->cb) {
		unsigned long idx = strtoul(next, NULL, 10);
		uint8_t rec[REC_BYTES];
		struct lsec_pairing p;

		if (idx > LSEC_MAX_SATS || read_cb(cb_arg, rec, len) != (ssize_t)len) {
			return 0;
		}
		decode(rec, &p);
		lsec_wipe(rec, sizeof(rec));

		ctx->cb(idx, &p, ctx->user);
		ctx->count++;
		lsec_wipe(&p, sizeof(p));
	}

	return 0;
}

int lsec_store_load(uint32_t *net_id, lsec_store_cb cb, void *user)
{
	struct load_ctx ctx = {.cb = cb, .user = user, .net_id = net_id};
	int ret;

	ret = settings_load_subtree_direct("lsec", load_one, &ctx);
	if (ret) {
		return ret;
	}

	/* The controller's network id is made once and then kept. */
	if (net_id && !ctx.have_net) {
		do {
			ret = lsec_random(net_id, sizeof(*net_id));
			if (ret) {
				return ret;
			}
		} while (*net_id == 0);

		ret = settings_save_one("lsec/net", net_id, sizeof(*net_id));
		if (ret) {
			return ret;
		}
	}

	return ctx.count;
}

int lsec_store_save(void *user, uint8_t slot, const struct lsec_pairing *p)
{
	char name[16];
	uint8_t rec[REC_BYTES];
	int ret;

	ARG_UNUSED(user);
	snprintf(name, sizeof(name), "lsec/p/%u", slot);

	if (!p) {
		return settings_delete(name);
	}

	encode(p, rec);
	ret = settings_save_one(name, rec, sizeof(rec));
	lsec_wipe(rec, sizeof(rec));

	return ret;
}

int lsec_store_clear(void)
{
	int ret = 0;

	for (uint8_t idx = 0; idx <= LSEC_MAX_SATS; idx++) {
		int err = lsec_store_save(NULL, idx, NULL);

		if (err && err != -ENOENT) {
			ret = err;
		}
	}

	return ret;
}
