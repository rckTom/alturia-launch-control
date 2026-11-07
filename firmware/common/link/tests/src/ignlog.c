/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * The ignition log on the wire: log polls, the index and chunks, and the
 * record summary of common/ignlog/ignlog_rec.c.
 */

#include <string.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "ignlog_rec.h"
#include "sat_wire.h"

ZTEST(ignlog_wire, test_log_poll_round_trip)
{
	struct wire_log_req req = { .op = WIRE_LOG_OP_CHUNK, .seq = 0x01020304, .off = 576 };
	struct wire_log_req got;
	uint8_t buf[WIRE_LOG_POLL_LEN];
	struct link_cmd cmd;

	zassert_equal(wire_encode_log_poll(&req, buf), WIRE_LOG_POLL_LEN);
	zassert_ok(wire_decode_log_poll(buf, sizeof(buf), &got));
	zassert_equal(got.op, req.op);
	zassert_equal(got.seq, req.seq);
	zassert_equal(got.off, req.off);

	/* Not a command poll, and a command poll is not a log poll */
	zassert_not_equal(wire_decode_poll(buf, sizeof(buf), 0, 1, &cmd, NULL), 0);
	buf[0] = 3;
	zassert_not_equal(wire_decode_log_poll(buf, sizeof(buf), &got), 0);
}

ZTEST(ignlog_wire, test_index_round_trip)
{
	struct wire_log_index ix = {
		.store_id = 0xbeef, .boots = 3, .now_ms = 123456, .scale_na = 7193359, .count = 2,
		.e = {
			{ .seq = 1, .t_fire_ms = 1000, .boot = 3, .n_samples = 1005, .period_us = 1000,
			  .peak_code = 245, .open_idx = 8, .channel = 1, .flags = 0 },
			{ .seq = 2, .t_fire_ms = 2000, .boot = 3, .n_samples = 125, .period_us = 1000,
			  .peak_code = 448, .open_idx = IGNLOG_OPEN_NONE, .channel = 5,
			  .flags = IGNLOG_F_FAULT },
		},
	};
	uint8_t buf[WIRE_LOG_INDEX_LEN(WIRE_LOG_INDEX_MAX)];
	struct wire_log_index got;
	size_t len = wire_encode_log_index(&ix, buf);

	zassert_equal(len, WIRE_LOG_INDEX_LEN(2));
	zassert_equal(wire_reply_kind(buf, len), WIRE_REPLY_LOG_INDEX);
	zassert_ok(wire_decode_log_index(buf, len, &got));
	zassert_equal(got.store_id, ix.store_id);
	zassert_equal(got.now_ms, ix.now_ms);
	zassert_equal(got.scale_na, ix.scale_na);
	zassert_equal(got.count, 2);
	zassert_mem_equal(&got.e[1], &ix.e[1], sizeof(ix.e[1]));
	zassert_not_equal(wire_decode_log_index(buf, len - 1, &got), 0);

	/* A status is still a status */
	struct sat_status st = { .seq = 5 };

	len = wire_encode_status(&st, false, buf);
	zassert_equal(wire_reply_kind(buf, len), WIRE_REPLY_STATUS);
}

ZTEST(ignlog_wire, test_chunk_round_trip)
{
	uint8_t data[WIRE_LOG_CHUNK_MAX];
	uint8_t buf[WIRE_LOG_CHUNK_LEN];
	struct wire_log_chunk c = { .seq = 9, .off = 192, .total = 2042, .len = sizeof(data),
				    .data = data };
	struct wire_log_chunk got;
	size_t len;

	for (size_t i = 0; i < sizeof(data); i++) {
		data[i] = i * 7;
	}
	len = wire_encode_log_chunk(&c, buf);
	zassert_equal(len, WIRE_LOG_CHUNK_LEN);
	zassert_equal(wire_reply_kind(buf, len), WIRE_REPLY_LOG_CHUNK);
	zassert_ok(wire_decode_log_chunk(buf, len, &got));
	zassert_equal(got.seq, 9);
	zassert_equal(got.off, 192);
	zassert_equal(got.total, 2042);
	zassert_equal(got.len, sizeof(data));
	zassert_mem_equal(got.data, data, sizeof(data));

	/* Past the record's end */
	sys_put_le16(100, buf + 7);
	zassert_not_equal(wire_decode_log_chunk(buf, len, &got), 0);
}

static void put_samples(uint8_t *rec, const uint16_t *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		sys_put_le16(s[i], rec + IGNLOG_HDR_LEN + 2 * i);
	}
}

ZTEST(ignlog_wire, test_summary)
{
	/* 7.19 mA per code: 245 is 1.76 A; 100 mA is 14 codes */
	static const uint16_t opens[] = { 0, 245, 240, 236, 3, 0, 1, 0, 0, 0 };
	static const uint16_t stays[] = { 0, 245, 245, 245, 245, 245, 245, 245, 245, 0 };
	static const uint16_t none[] = { 0, 2, 5, 3, 0, 0, 0, 0, 0, 0 };
	uint8_t rec[IGNLOG_REC_LEN(10)];
	struct ignlog_meta m = {
		.e = { .n_samples = 10, .period_us = 1000 },
		.pulse_ms = 8,
		.scale_na = 7193359,
	};
	struct ignlog_meta got;

	put_samples(rec, opens, 10);
	ignlog_summarize(rec, &m);
	zassert_equal(m.e.peak_code, 245);
	zassert_equal(m.e.open_idx, 4);
	zassert_false(m.e.flags & IGNLOG_F_NO_CURRENT);

	/* Current until the pulse ends at 8 ms: never opened */
	put_samples(rec, stays, 10);
	ignlog_summarize(rec, &m);
	zassert_equal(m.e.open_idx, IGNLOG_OPEN_NONE);

	put_samples(rec, none, 10);
	ignlog_summarize(rec, &m);
	zassert_true(m.e.flags & IGNLOG_F_NO_CURRENT);
	zassert_equal(m.e.open_idx, IGNLOG_OPEN_NONE);

	/* The header round trip */
	m.e.seq = 42;
	m.store_id = 0x1234;
	ignlog_meta_put(&m, rec);
	zassert_ok(ignlog_meta_get(rec, sizeof(rec), &got));
	zassert_equal(got.e.seq, 42);
	zassert_equal(got.store_id, 0x1234);
	zassert_equal(got.scale_na, m.scale_na);
	zassert_equal(got.e.flags, m.e.flags);
	rec[0] = 2;
	zassert_not_equal(ignlog_meta_get(rec, sizeof(rec), &got), 0);
}

ZTEST_SUITE(ignlog_wire, NULL, NULL, NULL, NULL, NULL);
