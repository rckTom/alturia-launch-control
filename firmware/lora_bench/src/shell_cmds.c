/* SPDX-License-Identifier: Apache-2.0 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include "bench.h"
#include "ctrl_link.h"
#include "radio.h"
#include "sat_link.h"
#include "secure.h"
#include "trace.h"

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t f = radio_frequency();

	shell_print(sh, "node %04x", bench_node_id());
	shell_print(sh, "%u.%03u MHz, SF%u, BW%u, %d dBm at the antenna",
		    f / 1000000, (f / 1000) % 1000, radio_spreading_factor(),
		    radio_bandwidth_khz(), radio_tx_power());
	shell_print(sh, "airtime %u ms per %zu-byte packet",
		    radio_airtime_ms(sizeof(struct bench_pkt)),
		    sizeof(struct bench_pkt));
	return 0;
}

static int cmd_ping(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t count = argc > 1 ? strtoul(argv[1], NULL, 0) : 10;
	uint32_t interval_ms = argc > 2 ? strtoul(argv[2], NULL, 0) : 2000;
	int ret;

	if (secure_status()->role != ROLE_OFF) {
		return secure_request(REQ_PING, 0, count, interval_ms);
	}

	ret = bench_ping_start(count, interval_ms);

	if (ret == -EBUSY) {
		shell_error(sh, "a ping run is in progress, \"bench stop\" ends it");
	}
	return ret;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	if (secure_status()->role != ROLE_OFF) {
		return secure_request(REQ_STOP, 0, 0, 0);
	}

	bench_ping_stop();
	return 0;
}

static const char *const role_names[] = {"off", "ctrl", "sat"};

static int cmd_role(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	if (argc == 1) {
		shell_print(sh, "%s", role_names[secure_status()->role]);
		return 0;
	}

	for (int r = ROLE_OFF; r <= ROLE_SAT; r++) {
		if (strcmp(argv[1], role_names[r]) == 0) {
			ret = secure_role_save(r);
			if (ret) {
				shell_error(sh, "save: %d", ret);
				return ret;
			}
			shell_print(sh, "role %s, pairings erased; rebooting", role_names[r]);
			k_sleep(K_MSEC(100));
			sys_reboot(SYS_REBOOT_COLD);
		}
	}

	shell_error(sh, "role is off, ctrl or sat");
	return -EINVAL;
}

/* The secure-mode commands only queue a request for the secure loop. */
static int request(const struct shell *sh, enum secure_req_type type, uint64_t uid,
		   uint32_t arg)
{
	int ret = secure_request(type, uid, arg, 0);

	if (ret == -ENOTSUP) {
		shell_error(sh, "needs a role: \"bench role ctrl\" or \"bench role sat\"");
	}
	return ret;
}

static int cmd_pair(const struct shell *sh, size_t argc, char **argv)
{
	uint64_t uid = argc > 1 ? strtoull(argv[1], NULL, 16) : 0;
	uint32_t slot = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;

	if (slot > LSEC_MAX_SATS) {
		shell_error(sh, "slots are 1 to %d", LSEC_MAX_SATS);
		return -EINVAL;
	}
	return request(sh, REQ_PAIR, uid, slot);
}

static int cmd_poll(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t count = argc > 1 ? strtoul(argv[1], NULL, 0) : 20;
	uint32_t interval_ms = argc > 2 ? strtoul(argv[2], NULL, 0) : 250;
	uint32_t phantoms = argc > 3 ? strtoul(argv[3], NULL, 0) : 0;
	uint32_t reply_len = argc > 4 ? strtoul(argv[4], NULL, 0) : 0;
	const struct secure_status *st = secure_status();

	if (st->role != ROLE_CTRL || !st->joined) {
		shell_error(sh, "needs the controller role and a joined satellite");
		return -ENOTCONN;
	}
	if (phantoms > 0xFF || (phantoms & BIT(st->slot - 1)) || (phantoms >> (st->slot - 1))) {
		shell_error(sh, "phantoms go below our satellite's slot %u: a mask of 0x%02x at most",
			    st->slot, (unsigned int)BIT_MASK(st->slot - 1));
		return -EINVAL;
	}

	return secure_request(REQ_POLL, phantoms | (reply_len << 8), count, interval_ms);
}

static int cmd_lbt(const struct shell *sh, size_t argc, char **argv)
{
	const struct secure_status *st = secure_status();
	int16_t floor = radio_noise_floor();
	int16_t peak;
	int clear = radio_channel_clear(LINK_LBT_LISTEN_US, LINK_LBT_MARGIN_DB, &peak);

	shell_print(sh, "noise floor %d dBm, busy above %d dBm, now %d dBm (%s)", floor,
		    floor + LINK_LBT_MARGIN_DB, peak, clear > 0 ? "clear" : "busy");

	if (st->role == ROLE_CTRL) {
		struct ctrl_link_state ls;

		ctrl_link_state(&ls);
		shell_print(sh, "polls %u, queue full %u, tx queue full %u", ls.polls,
			    ls.poll_q_full, ls.tx_q_full);
		shell_print(sh, "listens busy %u, sent anyway %u", ls.lbt.busy, ls.lbt.forced);
	} else if (st->role == ROLE_SAT) {
		struct sat_link_state ls;

		sat_link_state(&ls);
		shell_print(sh, "polls %u, replies %u, slots skipped: %u busy %u late", ls.polls,
			    ls.replies, ls.slot_busy, ls.slot_late);
		shell_print(sh, "listens busy %u, frames given up %u", ls.lbt.busy, ls.lbt.gave_up);
	}
	return 0;
}

static int cmd_jam(const struct shell *sh, size_t argc, char **argv)
{
	if (strcmp(argv[1], "slot") == 0) {
		bool on = argc < 3 || strcmp(argv[2], "off") != 0;

		trace_set_jam_slot(on);
		shell_print(sh, "reply slots after each poll %s", on ? "jammed" : "free");
		return 0;
	}

	return request(sh, REQ_JAM, 0, strtoul(argv[1], NULL, 0));
}

static int cmd_scan(const struct shell *sh, size_t argc, char **argv)
{
	bool on = argc < 2 || strcmp(argv[1], "off") != 0;

	return request(sh, REQ_SCAN, 0, on);
}

static int cmd_confirm(const struct shell *sh, size_t argc, char **argv)
{
	return request(sh, REQ_CONFIRM, 0, 0);
}

static int cmd_reject(const struct shell *sh, size_t argc, char **argv)
{
	return request(sh, REQ_REJECT, 0, 0);
}

static int cmd_unpair(const struct shell *sh, size_t argc, char **argv)
{
	return request(sh, REQ_UNPAIR, 0, 0);
}

static int cmd_replay(const struct shell *sh, size_t argc, char **argv)
{
	return request(sh, REQ_REPLAY, 0, 0);
}

static int cmd_link(const struct shell *sh, size_t argc, char **argv)
{
	const struct secure_status *st = secure_status();

	shell_print(sh, "role %s, %s, %s", role_names[st->role],
		    st->paired ? "paired" : "not paired", st->joined ? "joined" : "no session");
	if (st->uid) {
		shell_print(sh, "%s %016llx", st->role == ROLE_CTRL ? "satellite" : "uid",
			    st->uid);
	}
	shell_print(sh, "rx ok %u, bad tag %u, replay %u, foreign %u, no session %u, malformed %u",
		    st->stats.rx_ok, st->stats.bad_tag, st->stats.replay, st->stats.foreign,
		    st->stats.no_session, st->stats.malformed);
	return 0;
}

static int cmd_power(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		/* The radio caps it at the band's limit; the print shows what it applied. */
		radio_set_tx_power(CLAMP(strtol(argv[1], NULL, 0), INT8_MIN, INT8_MAX));
	}

	shell_print(sh, "%d dBm at the antenna", radio_tx_power());
	return 0;
}

static const char *const trace_names[] = {"off", "on", "hex"};

static int cmd_trace(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 1) {
		shell_print(sh, "trace %s", trace_names[trace_mode()]);
		return 0;
	}

	for (int m = TRACE_OFF; m <= TRACE_HEX; m++) {
		if (strcmp(argv[1], trace_names[m]) == 0) {
			trace_set_mode(m);
			if (secure_status()->role == ROLE_OFF) {
				shell_warn(sh, "role is off; only the secure roles are traced");
			}
			return 0;
		}
	}

	shell_error(sh, "trace is off, on or hex");
	return -EINVAL;
}

SHELL_STATIC_SUBCMD_SET_CREATE(bench_cmds,
	SHELL_CMD(info, NULL, "Show node id and radio settings", cmd_info),
	SHELL_CMD_ARG(ping, NULL, "ping [count] [interval_ms]", cmd_ping, 1, 2),
	SHELL_CMD(stop, NULL, "End the current ping run", cmd_stop),
	SHELL_CMD_ARG(power, NULL, "power [dBm]: show or set TX power", cmd_power, 1, 1),
	SHELL_CMD_ARG(role, NULL, "role [off|ctrl|sat]: secure-link role (reboots)",
		      cmd_role, 1, 1),
	SHELL_CMD_ARG(pair, NULL, "pair [uid] [slot]: satellite: pairing mode; "
		      "controller: pair with uid (0: the first one heard) into slot (8)",
		      cmd_pair, 1, 2),
	SHELL_CMD_ARG(scan, NULL, "scan [on|off]: controller: list satellites in pairing mode",
		      cmd_scan, 1, 1),
	SHELL_CMD(confirm, NULL, "The patterns match", cmd_confirm),
	SHELL_CMD(reject, NULL, "The patterns differ, or cancel pairing", cmd_reject),
	SHELL_CMD(unpair, NULL, "Forget the pairing", cmd_unpair),
	SHELL_CMD(replay, NULL, "Resend the last sealed DATA frame as-is", cmd_replay),
	SHELL_CMD(link, NULL, "Secure link state and drop counters", cmd_link),
	SHELL_CMD_ARG(poll, NULL,
		      "poll [count] [interval_ms] [phantom_mask] [reply_len]: controller: time "
		      "the satellite's replies in their slots",
		      cmd_poll, 1, 4),
	SHELL_CMD(lbt, NULL, "Noise floor, busy threshold and listen-before-talk counters",
		  cmd_lbt),
	SHELL_CMD_ARG(jam, NULL,
		      "jam <ms>: raw frames without listening first; jam slot [on|off]: "
		      "controller: jam the reply slots after each poll",
		      cmd_jam, 2, 1),
	SHELL_CMD_ARG(trace, NULL, "trace [off|on|hex]: log every frame sent and received",
		      cmd_trace, 1, 1),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(bench, &bench_cmds, "LoRa bench test", NULL);
