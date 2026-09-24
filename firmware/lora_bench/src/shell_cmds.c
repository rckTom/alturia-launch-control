/* SPDX-License-Identifier: Apache-2.0 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>

#include "bench.h"
#include "radio.h"
#include "secure.h"

/*
 * Ceiling for "bench power". 14 dBm is the EU limit in the 868.0-868.6 MHz
 * sub-band, and it keeps two boards on the same desk from overdriving each
 * other's LNA and SX1262 input.
 */
#define MAX_BENCH_POWER_DBM 14

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t f = radio_frequency();

	shell_print(sh, "node %04x", bench_node_id());
	shell_print(sh, "%u.%03u MHz, SF%u, BW125, %d dBm at the antenna",
		    f / 1000000, (f / 1000) % 1000, radio_spreading_factor(),
		    radio_tx_power());
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

	return request(sh, REQ_PAIR, uid, 0);
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
		long dbm = strtol(argv[1], NULL, 0);

		if (dbm > MAX_BENCH_POWER_DBM) {
			shell_error(sh, "limited to %d dBm", MAX_BENCH_POWER_DBM);
			return -EINVAL;
		}
		radio_set_tx_power(dbm);
	}

	shell_print(sh, "%d dBm at the antenna", radio_tx_power());
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(bench_cmds,
	SHELL_CMD(info, NULL, "Show node id and radio settings", cmd_info),
	SHELL_CMD_ARG(ping, NULL, "ping [count] [interval_ms]", cmd_ping, 1, 2),
	SHELL_CMD(stop, NULL, "End the current ping run", cmd_stop),
	SHELL_CMD_ARG(power, NULL, "power [dBm]: show or set TX power", cmd_power, 1, 1),
	SHELL_CMD_ARG(role, NULL, "role [off|ctrl|sat]: secure-link role (reboots)",
		      cmd_role, 1, 1),
	SHELL_CMD_ARG(pair, NULL, "pair [uid]: satellite: pairing mode; "
		      "controller: pair with uid, or the first one heard", cmd_pair, 1, 1),
	SHELL_CMD_ARG(scan, NULL, "scan [on|off]: controller: list satellites in pairing mode",
		      cmd_scan, 1, 1),
	SHELL_CMD(confirm, NULL, "The patterns match", cmd_confirm),
	SHELL_CMD(reject, NULL, "The patterns differ, or cancel pairing", cmd_reject),
	SHELL_CMD(unpair, NULL, "Forget the pairing", cmd_unpair),
	SHELL_CMD(replay, NULL, "Resend the last sealed DATA frame as-is", cmd_replay),
	SHELL_CMD(link, NULL, "Secure link state and drop counters", cmd_link),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(bench, &bench_cmds, "LoRa bench test", NULL);
