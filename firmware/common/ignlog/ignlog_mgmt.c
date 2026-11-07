/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * mcumgr group IGNLOG_MGMT_GROUP: list and read ignition log records, on
 * the satellite (its own) and on the controller (its copies). The SMP
 * frames travel inside the USB shell's byte stream, so the browser tool
 * can download while its terminal stays in use: every response is small
 * (at most READ_MAX bytes of record data) and the client sends one
 * request at a time. documentation/ignition_log.md has the commands.
 *
 *   0 list  (read)   {sat?}            -> {busy, sats?, sat?, store, boots, now,
 *                                          scale, records: [[seq, ch, flags, n,
 *                                          period, peak, open, t, boot, size,
 *                                          have], ...]}
 *   1 read  (read)   {sat?, seq, off}  -> {off, total, data}
 *   2 clear (write)  {sat?}            -> {}
 *   3 fetch (write)  {sat}             -> {}   controller only
 *
 * "sats" (controller, without "sat"): [[sat, store, count, have], ...]
 * for every slot with an index.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/sys/util.h>

#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>

#include <mgmt/mcumgr/util/zcbor_bulk.h>

#include "ignlog_mgmt.h"

/* Record bytes per read: with the CBOR and SMP header well inside the 640-byte netbuf. */
#define READ_MAX 256

BUILD_ASSERT(IGNLOG_MGMT_GROUP == MGMT_GROUP_ID_PERUSER);

static int mgmt_err(int err)
{
	switch (err) {
	case 0:
		return MGMT_ERR_EOK;
	case -ENOENT:
		return MGMT_ERR_ENOENT;
	case -EBUSY:
	case -EAGAIN:
		return MGMT_ERR_EBUSY;
	case -ENOTSUP:
		return MGMT_ERR_ENOTSUP;
	case -EINVAL:
		return MGMT_ERR_EINVAL;
	case -ENOTCONN:
		return MGMT_ERR_EBADSTATE;
	default:
		return MGMT_ERR_EUNKNOWN;
	}
}

/* Decode the optional "sat", "seq" and "off". */
static bool decode_args(zcbor_state_t *zsd, uint32_t *sat, uint32_t *seq, uint32_t *off)
{
	size_t decoded;
	struct zcbor_map_decode_key_val keys[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("sat", zcbor_uint32_decode, sat),
		ZCBOR_MAP_DECODE_KEY_DECODER("seq", zcbor_uint32_decode, seq),
		ZCBOR_MAP_DECODE_KEY_DECODER("off", zcbor_uint32_decode, off),
	};

	return zcbor_map_decode_bulk(zsd, keys, ARRAY_SIZE(keys), &decoded) == 0;
}

static bool put_u32(zcbor_state_t *zse, const char *key, size_t key_len, uint32_t v)
{
	return zcbor_tstr_encode_ptr(zse, key, key_len) && zcbor_uint32_put(zse, v);
}

#define PUT(zse, key, v) put_u32(zse, key, sizeof(key) - 1, v)

static int ignlog_mgmt_list(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	zcbor_state_t *zsd = ctxt->reader->zs;
	uint32_t sat = 0, seq = 0, off = 0;
	uint8_t sats = ignlog_be_sats();
	struct ignlog_store_view v;
	bool want_store;
	bool ok;
	int ret;

	if (!decode_args(zsd, &sat, &seq, &off) || sat > 8) {
		return MGMT_ERR_EINVAL;
	}

	/* The satellite always lists its own; the controller one slot if asked. */
	want_store = sats == 0 || sat != 0;
	ret = want_store ? ignlog_be_view(sat, &v) : 0;
	if (ret && ret != -ENOENT) {
		return mgmt_err(ret);
	}

	ok = PUT(zse, "busy", ignlog_be_busy());

	if (ok && sats) {
		uint32_t n = POPCOUNT(sats);

		ok = zcbor_tstr_put_lit(zse, "sats") && zcbor_list_start_encode(zse, n);
		for (uint8_t s = 1; ok && s <= 8; s++) {
			struct ignlog_store_view sv;

			if (!(sats & BIT(s - 1)) || ignlog_be_view(s, &sv)) {
				continue;
			}
			ok = zcbor_list_start_encode(zse, 4) && zcbor_uint32_put(zse, s) &&
			     zcbor_uint32_put(zse, sv.store_id) && zcbor_uint32_put(zse, sv.count) &&
			     zcbor_uint32_put(zse, sv.have) && zcbor_list_end_encode(zse, 4);
		}
		ok = ok && zcbor_list_end_encode(zse, n);
	}

	if (ok && want_store && ret == 0) {
		ok = PUT(zse, "sat", sat) && PUT(zse, "store", v.store_id) &&
		     PUT(zse, "boots", v.boots) && PUT(zse, "now", v.now_ms) &&
		     PUT(zse, "scale", v.scale_na) && zcbor_tstr_put_lit(zse, "records") &&
		     zcbor_list_start_encode(zse, v.count);
		for (int i = 0; ok && i < v.count; i++) {
			const struct ignlog_entry *e = &v.e[i];

			ok = zcbor_list_start_encode(zse, 11) && zcbor_uint32_put(zse, e->seq) &&
			     zcbor_uint32_put(zse, e->channel) && zcbor_uint32_put(zse, e->flags) &&
			     zcbor_uint32_put(zse, e->n_samples) &&
			     zcbor_uint32_put(zse, e->period_us) &&
			     zcbor_uint32_put(zse, e->peak_code) &&
			     zcbor_uint32_put(zse, e->open_idx) &&
			     zcbor_uint32_put(zse, e->t_fire_ms) && zcbor_uint32_put(zse, e->boot) &&
			     zcbor_uint32_put(zse, IGNLOG_REC_LEN(e->n_samples)) &&
			     zcbor_bool_put(zse, v.have & BIT(i)) && zcbor_list_end_encode(zse, 11);
		}
		ok = ok && zcbor_list_end_encode(zse, v.count);
	}

	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int ignlog_mgmt_read(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	zcbor_state_t *zsd = ctxt->reader->zs;
	uint32_t sat = 0, seq = 0, off = 0;
	static uint8_t buf[READ_MAX];
	size_t total = 0;
	int ret;
	bool ok;

	if (!decode_args(zsd, &sat, &seq, &off) || sat > 8 || seq == 0) {
		return MGMT_ERR_EINVAL;
	}

	/* One request at a time: mcumgr runs its handlers on one work queue. */
	ret = ignlog_be_read(sat, seq, off, buf, sizeof(buf), &total);
	if (ret < 0) {
		return mgmt_err(ret);
	}

	ok = PUT(zse, "off", off) && PUT(zse, "total", total) &&
	     zcbor_tstr_put_lit(zse, "data") && zcbor_bstr_encode_ptr(zse, (const char *)buf, ret);

	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int ignlog_mgmt_clear(struct smp_streamer *ctxt)
{
	uint32_t sat = 0, seq = 0, off = 0;

	if (!decode_args(ctxt->reader->zs, &sat, &seq, &off) || sat > 8) {
		return MGMT_ERR_EINVAL;
	}
	return mgmt_err(ignlog_be_clear(sat));
}

static int ignlog_mgmt_fetch(struct smp_streamer *ctxt)
{
	uint32_t sat = 0, seq = 0, off = 0;

	if (!decode_args(ctxt->reader->zs, &sat, &seq, &off) || sat < 1 || sat > 8) {
		return MGMT_ERR_EINVAL;
	}
	return mgmt_err(ignlog_be_fetch(sat));
}

static const struct mgmt_handler ignlog_mgmt_handlers[] = {
	[IGNLOG_MGMT_ID_LIST] = { ignlog_mgmt_list, NULL },
	[IGNLOG_MGMT_ID_READ] = { ignlog_mgmt_read, NULL },
	[IGNLOG_MGMT_ID_CLEAR] = { NULL, ignlog_mgmt_clear },
	[IGNLOG_MGMT_ID_FETCH] = { NULL, ignlog_mgmt_fetch },
};

static struct mgmt_group ignlog_mgmt_group = {
	.mg_handlers = ignlog_mgmt_handlers,
	.mg_handlers_count = ARRAY_SIZE(ignlog_mgmt_handlers),
	.mg_group_id = IGNLOG_MGMT_GROUP,
#ifdef CONFIG_MCUMGR_GRP_ENUM_DETAILS_NAME
	.mg_group_name = "ignlog",
#endif
};

static void ignlog_mgmt_register(void)
{
	mgmt_register_group(&ignlog_mgmt_group);
}

MCUMGR_HANDLER_DEFINE(ignlog_mgmt, ignlog_mgmt_register);
