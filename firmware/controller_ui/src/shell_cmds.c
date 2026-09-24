/*
 * Shell commands for driving the test bench:
 *   ctrl ...  controller-side inputs, same as the physical controls;
 *             "ctrl kbd" turns the terminal into the control panel
 *   sat ...   fault injection into the simulated satellites, and their
 *             side of pairing (the button gesture and confirmation)
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

/*
 * Keyboard mode: raw terminal input drives the controls directly, like the
 * keyboard of the native_sim build. A terminal only sends key presses, never
 * releases, so the FIRE button and the long push cannot be held: space
 * toggles FIRE and "e" stands in for the long push.
 */
static const char kbd_help[] =
	"Keyboard mode, q or Ctrl-C to leave\n"
	"  Up/Down  encoder    Enter  push    e  long push\n"
	"  k  arm key (toggle) a  ARM         s  SAFE\n"
	"  f  FILL             v  VENT        Space  FIRE (toggle)";

static enum { KBD_NORMAL, KBD_ESC, KBD_CSI } kbd_state;
static bool kbd_last_cr;

static void kbd_leave(const struct shell *sh)
{
	/* Never leave FIRE held behind */
	inputs_set_fire(false);
	shell_set_bypass(sh, NULL, NULL);
	shell_print(sh, "keyboard mode off");
}

static void kbd_key(const struct shell *sh, uint8_t c)
{
	switch (c) {
	case 'k':
		inputs_set_key(!inputs_key_on());
		shell_print(sh, "controller key %s", inputs_key_on() ? "ON" : "OFF");
		break;
	case ' ':
		inputs_set_fire(!inputs_fire_held());
		shell_print(sh, "FIRE %s", inputs_fire_held() ? "held" : "released");
		break;
	case 'a':
		ctrl_post(CTRL_REQ_ARM, 0);
		break;
	case 's':
		ctrl_post(CTRL_REQ_SAFE, 0);
		break;
	case 'f':
		ctrl_post(CTRL_REQ_FILL, 0);
		break;
	case 'v':
		ctrl_post(CTRL_REQ_VENT, 0);
		break;
	case 'e':
		ctrl_post(CTRL_REQ_EDIT, 0);
		break;
	case '\r':
	case '\n':
		ctrl_post(CTRL_REQ_SELECT, 0);
		break;
	default:
		break;
	}
}

static void kbd_bypass(const struct shell *sh, uint8_t *data, size_t len, void *user_data)
{
	ARG_UNUSED(user_data);

	for (size_t i = 0; i < len; i++) {
		uint8_t c = data[i];

		/* Arrow keys: ESC [ A/B, or ESC O A/B in application cursor mode */
		if (kbd_state == KBD_ESC) {
			kbd_state = (c == '[' || c == 'O') ? KBD_CSI : KBD_NORMAL;
			continue;
		}
		if (kbd_state == KBD_CSI) {
			/* Parameter bytes end at the final byte, 0x40..0x7e */
			if (c < 0x40 || c > 0x7e) {
				continue;
			}
			kbd_state = KBD_NORMAL;
			if (c == 'A' || c == 'B') {
				ctrl_post(CTRL_REQ_CURSOR, c == 'A' ? -1 : 1);
			}
			continue;
		}

		/* One select for CR LF */
		if (c == '\n' && kbd_last_cr) {
			kbd_last_cr = false;
			continue;
		}
		kbd_last_cr = (c == '\r');

		if (c == 0x1b) {
			kbd_state = KBD_ESC;
		} else if (c == 'q' || c == 0x03) {
			kbd_leave(sh);
			return;
		} else {
			kbd_key(sh, c);
		}
	}
}

static int cmd_ctrl_kbd(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	kbd_state = KBD_NORMAL;
	kbd_last_cr = false;
	shell_print(sh, "%s", kbd_help);
	shell_set_bypass(sh, kbd_bypass, NULL);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(ctrl_cmds,
	SHELL_CMD(kbd, NULL, "Keyboard mode: drive the controls with single keys", cmd_ctrl_kbd),
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

static int cmd_sat_pair(const struct shell *sh, size_t argc, char **argv)
{
	int sat = parse_sat(sh, argv[1]);

	ARG_UNUSED(argc);
	if (sat < 0) {
		return sat;
	}
	sat_sim_pair(sat);
	return 0;
}

static int cmd_sat_confirm(const struct shell *sh, size_t argc, char **argv)
{
	int sat = parse_sat(sh, argv[1]);

	ARG_UNUSED(argc);
	if (sat < 0) {
		return sat;
	}
	sat_sim_pair_confirm(sat, argv[0][0] == 'c');
	return 0;
}

static int cmd_sat_unpair(const struct shell *sh, size_t argc, char **argv)
{
	int sat = parse_sat(sh, argv[1]);

	ARG_UNUSED(argc);
	if (sat < 0) {
		return sat;
	}
	sat_sim_unpair(sat);
	return 0;
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
	SHELL_CMD_ARG(pair, NULL, "Pairing gesture: enter pairing mode <a|b>", cmd_sat_pair, 2, 0),
	SHELL_CMD_ARG(confirm, NULL, "Satellite user: LEDs match the controller <a|b>",
		      cmd_sat_confirm, 2, 0),
	SHELL_CMD_ARG(reject, NULL, "Satellite user: LEDs differ <a|b>", cmd_sat_confirm, 2, 0),
	SHELL_CMD_ARG(unpair, NULL, "Satellite forgets its controller <a|b>", cmd_sat_unpair, 2,
		      0),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(sat, &sat_cmds, "Simulated satellites", NULL);

#endif /* CONFIG_APP_SAT_SIM */
