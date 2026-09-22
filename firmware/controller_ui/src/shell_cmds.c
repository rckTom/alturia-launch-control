/*
 * Shell commands for driving the test bench:
 *   ctrl ...  controller-side inputs, same as the physical controls
 *   sat ...   fault injection into the simulated satellites
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/shell/shell.h>

#include "controller.h"
#include "inputs.h"
#include "link.h"
#include "pads.h"
#include "sat_sim.h"

static int parse_on_off(const struct shell *sh, const char *arg, bool *out)
{
	if (!strcmp(arg, "on") || !strcmp(arg, "arm") || !strcmp(arg, "1")) {
		*out = true;
	} else if (!strcmp(arg, "off") || !strcmp(arg, "safe") || !strcmp(arg, "0")) {
		*out = false;
	} else {
		shell_error(sh, "expected on/off");
		return -EINVAL;
	}
	return 0;
}

/* Parses a 1-based number and returns it 0-based, or -EINVAL */
static int parse_index(const struct shell *sh, const char *arg, int count, const char *what)
{
	int i = atoi(arg);

	if (i < 1 || i > count) {
		shell_error(sh, "%s must be 1..%d", what, count);
		return -EINVAL;
	}
	return i - 1;
}

static int cmd_ctrl_key(const struct shell *sh, size_t argc, char **argv)
{
	bool on;

	ARG_UNUSED(argc);
	if (parse_on_off(sh, argv[1], &on)) {
		return -EINVAL;
	}
	inputs_set_key(on);
	return 0;
}

static int cmd_ctrl_fire(const struct shell *sh, size_t argc, char **argv)
{
	bool held;

	ARG_UNUSED(argc);
	if (parse_on_off(sh, argv[1], &held)) {
		return -EINVAL;
	}
	inputs_set_fire(held);
	return 0;
}

static int cmd_ctrl_select(const struct shell *sh, size_t argc, char **argv)
{
	int p = parse_index(sh, argv[1], NUM_PADS, "pad");

	ARG_UNUSED(argc);
	if (p < 0) {
		return p;
	}
	ctrl_post(CTRL_REQ_TOGGLE_PAD, p);
	return 0;
}

static int cmd_ctrl_turn(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ctrl_post(CTRL_REQ_CURSOR, atoi(argv[1]));
	return 0;
}

#define DEFINE_CTRL_BUTTON(name, req)                                             \
	static int cmd_ctrl_##name(const struct shell *sh, size_t argc, char **argv) \
	{                                                                           \
		ARG_UNUSED(sh);                                                     \
		ARG_UNUSED(argc);                                                   \
		ARG_UNUSED(argv);                                                   \
		ctrl_post(req, 0);                                                  \
		return 0;                                                           \
	}

DEFINE_CTRL_BUTTON(push, CTRL_REQ_SELECT)
DEFINE_CTRL_BUTTON(hold, CTRL_REQ_EDIT)
DEFINE_CTRL_BUTTON(arm, CTRL_REQ_ARM)
DEFINE_CTRL_BUTTON(safe, CTRL_REQ_SAFE)
DEFINE_CTRL_BUTTON(fill, CTRL_REQ_FILL)
DEFINE_CTRL_BUTTON(vent, CTRL_REQ_VENT)

SHELL_STATIC_SUBCMD_SET_CREATE(ctrl_cmds,
	SHELL_CMD_ARG(key, NULL, "Controller arm key <on|off>", cmd_ctrl_key, 2, 0),
	SHELL_CMD_ARG(select, NULL, "Toggle pad selection <1-7>", cmd_ctrl_select, 2, 0),
	SHELL_CMD_ARG(turn, NULL, "Turn the encoder <steps, negative is up>", cmd_ctrl_turn, 2, 0),
	SHELL_CMD(push, NULL, "Push the encoder (toggle pad under cursor)", cmd_ctrl_push),
	SHELL_CMD(hold, NULL, "Long-push the encoder (open / save label editor)", cmd_ctrl_hold),
	SHELL_CMD(arm, NULL, "Press ARM", cmd_ctrl_arm),
	SHELL_CMD(fill, NULL, "Press FILL", cmd_ctrl_fill),
	SHELL_CMD(vent, NULL, "Press VENT", cmd_ctrl_vent),
	SHELL_CMD_ARG(fire, NULL, "Hold/release FIRE button <on|off>", cmd_ctrl_fire, 2, 0),
	SHELL_CMD(safe, NULL, "Press SAFE", cmd_ctrl_safe),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(ctrl, &ctrl_cmds, "Controller inputs", NULL);

#ifdef CONFIG_APP_SAT_SIM

static struct shell *print_sh;

static void shell_print_cb(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	shell_vfprintf(print_sh, SHELL_NORMAL, fmt, ap);
	va_end(ap);
}

/* Satellite by letter (a, b) */
static int parse_sat(const struct shell *sh, const char *arg)
{
	int sat = tolower((unsigned char)arg[0]) - 'a';

	if (arg[1] != '\0' || sat < 0 || sat >= NUM_SATS) {
		shell_error(sh, "satellite must be a..%c", 'a' + NUM_SATS - 1);
		return -EINVAL;
	}
	return sat;
}

static int cmd_sat_status(const struct shell *sh, size_t argc, char **argv)
{
	print_sh = (struct shell *)sh;

	if (argc > 1) {
		int sat = parse_sat(sh, argv[1]);

		if (sat < 0) {
			return sat;
		}
		sat_sim_print(sat, shell_print_cb);
		return 0;
	}
	for (int sat = 0; sat < NUM_SATS; sat++) {
		sat_sim_print(sat, shell_print_cb);
	}
	return 0;
}

#define DEFINE_SAT_BOOL(name, fn)                                                 \
	static int cmd_sat_##name(const struct shell *sh, size_t argc, char **argv) \
	{                                                                           \
		int sat = parse_sat(sh, argv[1]);                                   \
		bool on;                                                            \
                                                                                    \
		ARG_UNUSED(argc);                                                   \
		if (sat < 0 || parse_on_off(sh, argv[2], &on)) {                    \
			return -EINVAL;                                             \
		}                                                                   \
		fn(sat, on);                                                        \
		return 0;                                                           \
	}

DEFINE_SAT_BOOL(link, sat_sim_set_link)
DEFINE_SAT_BOOL(key, sat_sim_set_key)
DEFINE_SAT_BOOL(estop, sat_sim_set_estop)

static int cmd_sat_loss(const struct shell *sh, size_t argc, char **argv)
{
	int sat = parse_sat(sh, argv[1]);

	ARG_UNUSED(argc);
	if (sat < 0) {
		return sat;
	}
	sat_sim_set_loss(sat, atoi(argv[2]));
	return 0;
}

static int cmd_sat_vbat(const struct shell *sh, size_t argc, char **argv)
{
	int sat = parse_sat(sh, argv[1]);

	ARG_UNUSED(argc);
	if (sat < 0) {
		return sat;
	}
	sat_sim_set_vbat(sat, atoi(argv[2]));
	return 0;
}

static int cmd_sat_res(const struct shell *sh, size_t argc, char **argv)
{
	int sat = parse_sat(sh, argv[1]);
	int ch = parse_index(sh, argv[2], NUM_CHANNELS, "channel");

	ARG_UNUSED(argc);
	if (sat < 0 || ch < 0) {
		return -EINVAL;
	}
	if (!strcmp(argv[3], "open")) {
		sat_sim_set_res(sat, ch, RES_OPEN_MOHM);
	} else {
		sat_sim_set_res(sat, ch, atoi(argv[3]));
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sat_cmds,
	SHELL_CMD_ARG(status, NULL, "Show simulated satellites [a|b]", cmd_sat_status, 1, 1),
	SHELL_CMD_ARG(link, NULL, "Radio link <a|b> <on|off>", cmd_sat_link, 3, 0),
	SHELL_CMD_ARG(loss, NULL, "Packet loss <a|b> <percent>", cmd_sat_loss, 3, 0),
	SHELL_CMD_ARG(key, NULL, "Pad key switch <a|b> <arm|safe>", cmd_sat_key, 3, 0),
	SHELL_CMD_ARG(estop, NULL, "Pad E-STOP <a|b> <on|off>", cmd_sat_estop, 3, 0),
	SHELL_CMD_ARG(res, NULL, "Channel load <a|b> <1-6> <mOhm|open>", cmd_sat_res, 4, 0),
	SHELL_CMD_ARG(vbat, NULL, "Pack voltage <a|b> <mV>", cmd_sat_vbat, 3, 0),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(sat, &sat_cmds, "Simulated satellites", NULL);

#endif /* CONFIG_APP_SAT_SIM */
