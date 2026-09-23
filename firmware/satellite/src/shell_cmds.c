/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>

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

static int cmd_key(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t key[32] = {0};
	int ret = keys_provision(key, sizeof(key));

	shell_print(sh, "provision: %d", ret);
	return ret;
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
	SHELL_CMD_ARG(key, NULL, "provision a zero key (bring-up only)", cmd_key, 1, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(sat, &sat_cmds, "Satellite control", NULL);
