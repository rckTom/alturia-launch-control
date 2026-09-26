/* SPDX-License-Identifier: Apache-2.0 */

/*
 * sat_wire: polls that ask for the short or the full status, and both
 * status sizes, merged into the last status received.
 */

#include <string.h>
#include <zephyr/ztest.h>

#include "lsec.h"
#include "sat_wire.h"

static struct sat_status sample(void)
{
	struct sat_status st = {
		.seq = 1234,
		.state = SAT_ARMED,
		.key_armed = true,
		.estop = false,
		.armed_mask = 0x05,
		.firing_mask = 0x01,
		.valve_mask = 0x04,
		.fire_count = 7,
		.arm_refused = 0x20,
		.cmd_id = 99,
		.res_mohm = {1500, 2000, RES_OPEN_MOHM, 0, 12, 34},
		.cont = {CONT_OK, CONT_OK, CONT_OPEN, CONT_SHORT, CONT_OK, CONT_OK},
		.vbat_mv = 12340,
		.rail_ma = 250,
		.rssi_dbm = -97,
		.snr_db = -4,
	};

	return st;
}

static void assert_short_fields(const struct sat_status *got, const struct sat_status *want)
{
	zassert_equal(got->seq, want->seq);
	zassert_equal(got->state, want->state);
	zassert_equal(got->key_armed, want->key_armed);
	zassert_equal(got->estop, want->estop);
	zassert_equal(got->armed_mask, want->armed_mask);
	zassert_equal(got->firing_mask, want->firing_mask);
	zassert_equal(got->valve_mask, want->valve_mask);
	zassert_equal(got->fire_count, want->fire_count);
	zassert_equal(got->arm_refused, want->arm_refused);
	zassert_equal(got->cmd_id, want->cmd_id);
	zassert_mem_equal(got->cont, want->cont, sizeof(want->cont));
	zassert_equal(got->vbat_mv, want->vbat_mv);
}

ZTEST(sat_wire, test_full_round_trip)
{
	struct sat_status st = sample();
	struct sat_status got = {.seq = 1200};
	uint8_t buf[WIRE_STATUS_LEN];

	zassert_equal(wire_encode_status(&st, true, buf), WIRE_STATUS_LEN);
	zassert_ok(wire_decode_status(buf, WIRE_STATUS_LEN, &got));
	assert_short_fields(&got, &st);
	zassert_mem_equal(got.res_mohm, st.res_mohm, sizeof(st.res_mohm));
	zassert_equal(got.rail_ma, st.rail_ma);
	zassert_equal(got.rssi_dbm, st.rssi_dbm);
	zassert_equal(got.snr_db, st.snr_db);
}

ZTEST(sat_wire, test_short_keeps_full_only_fields)
{
	struct sat_status full = sample();
	struct sat_status st = sample();
	struct sat_status got = {.seq = 1200};
	uint8_t buf[WIRE_STATUS_LEN];

	zassert_ok(wire_decode_status(buf, wire_encode_status(&full, true, buf), &got));

	/* The satellite moves on; the short status carries only part of it. */
	st.seq++;
	st.state = SAT_SAFE;
	st.armed_mask = 0;
	st.cont[2] = CONT_OK;
	st.res_mohm[2] = 1800;
	st.rail_ma = 10;
	st.rssi_dbm = -60;

	zassert_equal(wire_encode_status(&st, false, buf), WIRE_STATUS_SHORT_LEN);
	zassert_ok(wire_decode_status(buf, WIRE_STATUS_SHORT_LEN, &got));
	assert_short_fields(&got, &st);
	zassert_equal(got.res_mohm[2], RES_OPEN_MOHM, "full-only field overwritten");
	zassert_equal(got.rail_ma, 250);
	zassert_equal(got.rssi_dbm, -97);
}

ZTEST(sat_wire, test_seq_extends_across_wrap)
{
	struct sat_status st = sample();
	struct sat_status got = {.seq = 0x1fff0};
	uint8_t buf[WIRE_STATUS_LEN];
	uint32_t before;

	/* The stored seq is 32 bits; only the low 16 travel. */
	st.seq = 0x1fffe;
	zassert_ok(wire_decode_status(buf, wire_encode_status(&st, false, buf), &got));
	zassert_equal(got.seq, 0x1fffe);
	before = got.seq;

	st.seq = 0x20003;
	zassert_ok(wire_decode_status(buf, wire_encode_status(&st, false, buf), &got));
	zassert_equal(got.seq, 0x20003, "wrap lost: %x", got.seq);
	zassert_true((int32_t)(got.seq - before) > 0);
}

ZTEST(sat_wire, test_bad_status_refused)
{
	struct sat_status st = sample();
	struct sat_status got = {0};
	uint8_t buf[WIRE_STATUS_LEN];
	size_t len = wire_encode_status(&st, false, buf);

	zassert_not_ok(wire_decode_status(buf, len - 1, &got), "short status cut off");
	zassert_not_ok(wire_decode_status(buf, WIRE_STATUS_LEN, &got), "type does not match length");
	buf[3] = SAT_FIRING + 1;
	zassert_not_ok(wire_decode_status(buf, len, &got), "bad state");
}

ZTEST(sat_wire, test_poll_full_flag)
{
	struct link_cmd cmds[3] = {
		{.type = CMD_HEARTBEAT},
		{.type = CMD_ARM, .mask = 0x03, .valves = 0x01, .id = 17},
		{.type = CMD_FIRE, .mask = 0x01, .id = 18},
	};
	uint8_t buf[WIRE_POLL_MAX_LEN(3)];
	struct link_cmd cmd;
	bool full = true;
	size_t len = wire_encode_poll(cmds, 3, 0x06, false, buf);

	zassert_equal(len, 1 + 2 * WIRE_POLL_CMD_LEN);
	zassert_ok(wire_decode_poll(buf, len, 0, 2, &cmd, &full));
	zassert_false(full);
	zassert_equal(cmd.type, CMD_ARM);
	zassert_equal(cmd.id, 17);
	zassert_ok(wire_decode_poll(buf, len, 1, 2, &cmd, NULL));
	zassert_equal(cmd.type, CMD_FIRE);

	len = wire_encode_poll(cmds, 3, 0x06, true, buf);
	zassert_ok(wire_decode_poll(buf, len, 1, 2, &cmd, &full));
	zassert_true(full);

	/* Replies must fit: the short status is what makes slots short. */
	zassert_true(WIRE_STATUS_SHORT_LEN + LSEC_OVERHEAD <= 33);
}

ZTEST_SUITE(sat_wire, NULL, NULL, NULL, NULL, NULL);
