/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <string.h>

#include "sat_link.h"
#include "satellite.h"

static int cmd_arm(const struct shell *sh, size_t argc, char **argv)
{
	int ch = atoi(argv[1]);
	int ret = channels_arm(ch, strtol(argv[2], NULL, 10) != 0);

	shell_print(sh, "arm ch%d: %d", ch, ret);
	return ret;
}

static int cmd_cont(const struct shell *sh, size_t argc, char **argv)
{
	int ch = atoi(argv[1]);
	int32_t mv;
	int ret = channels_continuity(ch, &mv);

	shell_print(sh, "ch%d continuity: %d mV (%d)", ch, mv, ret);
	return ret;
}

static int cmd_pair(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	if (argc == 1) {
		ret = pair_ui_start();
		shell_print(sh, "pairing mode: %d", ret);
		return ret;
	}

	/* For boards without the button or the LEDs: the log has the pattern. */
	if (strcmp(argv[1], "confirm") == 0 || strcmp(argv[1], "reject") == 0) {
		sat_link_pair_confirm(argv[1][0] == 'c');
		return 0;
	}

	shell_error(sh, "pair [confirm|reject]");
	return -EINVAL;
}

static int cmd_unpair(const struct shell *sh, size_t argc, char **argv)
{
	int ret = sat_link_unpair();

	shell_print(sh, "unpair: %d", ret);
	return ret;
}

static int cmd_link(const struct shell *sh, size_t argc, char **argv)
{
	struct sat_link_state ls;

	sat_link_state(&ls);
	shell_print(sh, "uid %016llx, %s, %s%s", ls.uid,
		    ls.paired ? "paired" : "not paired", ls.joined ? "joined" : "no session",
		    ls.pairing ? ", pairing" : "");
	if (ls.paired) {
		shell_print(sh, "slot %u", ls.slot);
	}
	shell_print(sh, "rx ok %u, bad tag %u, replay %u, foreign %u, no session %u, malformed %u",
		    ls.stats.rx_ok, ls.stats.bad_tag, ls.stats.replay, ls.stats.foreign,
		    ls.stats.no_session, ls.stats.malformed);
	return 0;
}

static int cmd_rail(const struct shell *sh, size_t argc, char **argv)
{
	int32_t mv, ma;
	int ret = rail_read(&mv, &ma);

	shell_print(sh, "rail: %d mV, %d mA (%d)", mv, ma, ret);
	return ret;
}

static int cmd_diag(const struct shell *sh, size_t argc, char **argv)
{
	int ret = channels_diagnostics(strtol(argv[1], NULL, 10) != 0);

	shell_print(sh, "diagnostics: %d", ret);
	return ret;
}

static int cmd_clear(const struct shell *sh, size_t argc, char **argv)
{
	int ret = channels_latch_clear();

	shell_print(sh, "latch clear: %d", ret);
	return ret;
}

static int cmd_fire(const struct shell *sh, size_t argc, char **argv)
{
	int ch = atoi(argv[1]);
	int ret = channels_fire(ch, strtoul(argv[2], NULL, 10));

	shell_print(sh, "fire ch%d: %d", ch, ret);
	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sat_cmds,
	SHELL_CMD_ARG(arm, NULL, "<ch> <0|1>", cmd_arm, 3, 0),
	SHELL_CMD_ARG(cont, NULL, "<ch>", cmd_cont, 2, 0),
	SHELL_CMD_ARG(fire, NULL, "<ch> <ms>", cmd_fire, 3, 0),
	SHELL_CMD_ARG(rail, NULL, "read the INA226", cmd_rail, 1, 0),
	SHELL_CMD_ARG(diag, NULL, "<0|1> TPS1HB08 diagnostics", cmd_diag, 2, 0),
	SHELL_CMD_ARG(clear, NULL, "clear the high-side latch", cmd_clear, 1, 0),
	SHELL_CMD_ARG(pair, NULL, "[confirm|reject] enter pairing mode (SAFE only), "
		      "or answer the pattern", cmd_pair, 1, 1),
	SHELL_CMD_ARG(unpair, NULL, "forget the console", cmd_unpair, 1, 0),
	SHELL_CMD_ARG(link, NULL, "link state and drop counters", cmd_link, 1, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(sat, &sat_cmds, "Satellite control", NULL);
