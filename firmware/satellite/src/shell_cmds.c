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
	struct cont_result res;
	int32_t mv;
	int ret = channels_continuity_mohm(ch, &res);

	if (ret == -ENOTSUP) {
		ret = channels_continuity(ch, &mv);
		shell_print(sh, "ch%d continuity: %d mV (%d)", ch, mv, ret);
		return ret;
	}
	if (ret && ret != -ERANGE) {
		shell_error(sh, "ch%d continuity: %d%s", ch, ret,
			    ret == -EBUSY ? " (armed)" : ret == -EIO ? " (no V_TEST)" : "");
		return ret;
	}
	if (ret == -ERANGE) {
		shell_warn(sh, "readings do not fit the circuit (diode drop out of range)");
	}

	if (res.open) {
		shell_print(sh, "ch%d: open", ch);
	} else {
		shell_print(sh, "ch%d: %d.%03d ohm", ch, res.mohm / 1000, res.mohm % 1000);
	}
	shell_print(sh, "  PYRO_H %d uV, diode %d mV, %d uA through the load",
		    res.pyro_h_uv, res.diode_uv / 1000, res.current_na / 1000);
	if (res.v_test_from >= 0) {
		shell_print(sh, "  V_TEST %d mV measured on ch%d (R12 model: %d mV)",
			    res.v_test_uv / 1000, res.v_test_from, res.v_test_model_uv / 1000);
	} else {
		shell_print(sh, "  V_TEST %d mV from the R12 model (no idle channel)",
			    res.v_test_uv / 1000);
	}
	shell_print(sh, "  ADC zero %d uV at the tap (subtracted)", res.zero_uv);
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
	shell_print(sh, "polls %u, replies %u, slots skipped: %u busy %u late", ls.polls,
		    ls.replies, ls.slot_busy, ls.slot_late);
	shell_print(sh, "channel %u%% busy, csma: senses busy %u, deferred %u, given up %u",
		    ls.util_pct, ls.csma.busy, ls.csma.deferred, ls.csma.gave_up);
	shell_print(sh, "rx ok %u, bad tag %u, replay %u, foreign %u, no session %u, malformed %u",
		    ls.stats.rx_ok, ls.stats.bad_tag, ls.stats.replay, ls.stats.foreign,
		    ls.stats.no_session, ls.stats.malformed);
	return 0;
}

static int cmd_rails(const struct shell *sh, size_t argc, char **argv)
{
	if (rails_count() == 0) {
		shell_print(sh, "no rail sensing on this board");
		return 0;
	}

	for (size_t i = 0; i < rails_count(); i++) {
		const char *name;
		int32_t mv;
		int ret = rails_read(i, &name, &mv);

		if (ret) {
			shell_print(sh, "%-14s error %d", name, ret);
		} else {
			shell_print(sh, "%-14s %3d.%03d V", name, mv / 1000, mv % 1000);
		}
	}
	return 0;
}

static int cmd_k1(const struct shell *sh, size_t argc, char **argv)
{
	int ret = 0;

	if (argc > 1) {
		if (!strcmp(argv[1], "on")) {
			ret = arm_relay_set(true);
		} else if (!strcmp(argv[1], "off")) {
			ret = arm_relay_set(false);
		} else {
			shell_error(sh, "on or off");
			return -EINVAL;
		}
		if (ret) {
			shell_error(sh, "arm relay: %d", ret);
			return ret;
		}
		/* Let K1 pull in or drop out before reading the rail. */
		k_msleep(50);
	}

	int key = arm_key_on();

	shell_print(sh, "arm relay K1: %s, key: %s", arm_relay_on() ? "pumping" : "off",
		    key < 0 ? "?" : (key ? "on (VKEY)" : "off"));

	for (size_t i = 0; i < rails_count(); i++) {
		const char *name;
		int32_t mv;

		if (rails_read(i, &name, &mv) == 0 && !strcmp(name, "V_PYRO_ARMED")) {
			shell_print(sh, "%s %d.%03d V", name, mv / 1000, mv % 1000);
		}
	}
	return 0;
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

static int cmd_fault(const struct shell *sh, size_t argc, char **argv)
{
	fault_print(sh);
	return 0;
}

static int cmd_beep(const struct shell *sh, size_t argc, char **argv)
{
	chime_startup();
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sat_cmds,
	SHELL_CMD_ARG(arm, NULL, "<ch> <0|1>", cmd_arm, 3, 0),
	SHELL_CMD_ARG(cont, NULL, "<ch> resistance across the output (ch 0-5 = panel 1-6)", cmd_cont, 2, 0),
	SHELL_CMD_ARG(fire, NULL, "<ch> <ms>", cmd_fire, 3, 0),
	SHELL_CMD_ARG(k1, NULL, "[on|off] arm relay K1 via ARM_ENABLE and ARM_PUMP", cmd_k1, 1, 1),
	SHELL_CMD_ARG(rails, NULL, "measure the supply rail voltages", cmd_rails, 1, 0),
	SHELL_CMD_ARG(diag, NULL, "<0|1> TPS1HB08 diagnostics", cmd_diag, 2, 0),
	SHELL_CMD_ARG(clear, NULL, "clear the high-side latch", cmd_clear, 1, 0),
	SHELL_CMD_ARG(pair, NULL, "[confirm|reject] enter pairing mode (SAFE only), "
		      "or answer the pattern", cmd_pair, 1, 1),
	SHELL_CMD_ARG(unpair, NULL, "forget the console", cmd_unpair, 1, 0),
	SHELL_CMD_ARG(link, NULL, "link state and drop counters", cmd_link, 1, 0),
	SHELL_CMD_ARG(beep, NULL, "play the start-up chime", cmd_beep, 1, 0),
	SHELL_CMD_ARG(fault, NULL, "the fatal error that rebooted the satellite", cmd_fault, 1, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(sat, &sat_cmds, "Satellite control", NULL);
