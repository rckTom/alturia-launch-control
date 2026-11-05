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

#include "battery.h"
#include "controller.h"
#include "ctrl_link.h"
#include "inputs.h"
#include "link.h"
#include "pads.h"
#include "sat_sim.h"
#include "sleep.h"

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

static int cmd_ctrl_up(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ctrl_post(CTRL_REQ_CURSOR, argc > 1 ? -atoi(argv[1]) : -1);
	return 0;
}

static int cmd_ctrl_down(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ctrl_post(CTRL_REQ_CURSOR, argc > 1 ? atoi(argv[1]) : 1);
	return 0;
}

static int cmd_ctrl_left(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ctrl_post(CTRL_REQ_ADJUST, argc > 1 ? -atoi(argv[1]) : -1);
	return 0;
}

static int cmd_ctrl_right(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ctrl_post(CTRL_REQ_ADJUST, argc > 1 ? atoi(argv[1]) : 1);
	return 0;
}

static int cmd_ctrl_battery(const struct shell *sh, size_t argc, char **argv)
{
	struct battery_info b;

	if (argc == 3) {
		bool charging;

		if (parse_on_off(sh, argv[2], &charging)) {
			return -EINVAL;
		}
		if (battery_fake(atoi(argv[1]), charging)) {
			shell_error(sh, "this board has a fuel gauge");
			return -EBUSY;
		}
		return 0;
	}

	battery_get(&b);
	if (b.gauge) {
		shell_print(sh, "gauge: %u %%, %u mV, to empty %u min, to full %u min", b.soc_pct,
			    b.voltage_mv, b.to_empty_min, b.to_full_min);
	} else {
		shell_print(sh, "gauge: no reading");
	}
	if (b.charger) {
		shell_print(sh, "charger: %s, health %s, input %s", battery_status_text(b.status),
			    battery_health_text(b.health), b.online ? "online" : "offline");
		shell_print(sh, "  fast charge %u mA to %u mV, precharge %u mA, termination %u mA",
			    b.ichg_ma, b.vreg_mv, b.iprechg_ma, b.iterm_ma);
		shell_print(sh, "  input limit %u mA, input min. %u mV", b.iinlim_ma, b.vindpm_mv);
	} else {
		shell_print(sh, "charger: no reading");
	}
	if (b.pgood >= 0) {
		shell_print(sh, "USB power good: %s", b.pgood ? "yes" : "no");
	}
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

DEFINE_CTRL_BUTTON(ok, CTRL_REQ_SELECT)
DEFINE_CTRL_BUTTON(hold, CTRL_REQ_EDIT)
DEFINE_CTRL_BUTTON(arm, CTRL_REQ_ARM)
DEFINE_CTRL_BUTTON(back, CTRL_REQ_BACK)
DEFINE_CTRL_BUTTON(back_hold, CTRL_REQ_BACK_LONG)
DEFINE_CTRL_BUTTON(fill, CTRL_REQ_FILL)
DEFINE_CTRL_BUTTON(vent, CTRL_REQ_VENT)
DEFINE_CTRL_BUTTON(power, CTRL_REQ_POWER)

/*
 * Keyboard mode: raw terminal input drives the controls directly, like the
 * keyboard of the native_sim build. A terminal only sends key presses, never
 * releases, so FIRE and the long presses cannot be held: space toggles
 * FIRE, "e" stands in for holding OK and "B" for holding BACK.
 */
static const char kbd_help[] =
	"Keyboard mode, q or Ctrl-C to leave\n"
	"  Arrows  D-pad       Enter  OK      e  hold OK\n"
	"  b  BACK             B  hold BACK   a  ARM\n"
	"  f  FILL             v  VENT        Space  FIRE (toggle)\n"
	"  p  power button";

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
	case ' ':
		inputs_set_fire(!inputs_fire_held());
		shell_print(sh, "FIRE %s", inputs_fire_held() ? "held" : "released");
		break;
	case 'a':
		ctrl_post(CTRL_REQ_ARM, 0);
		break;
	case 'b':
		ctrl_post(CTRL_REQ_BACK, 0);
		break;
	case 'B':
		ctrl_post(CTRL_REQ_BACK_LONG, 0);
		break;
	case 'p':
		ctrl_post(CTRL_REQ_POWER, 0);
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

		/* Arrow keys: ESC [ A-D, or ESC O A-D in application cursor mode */
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
			} else if (c == 'C' || c == 'D') {
				ctrl_post(CTRL_REQ_ADJUST, c == 'D' ? -1 : 1);
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

static int cmd_ctrl_sleep(const struct shell *sh, size_t argc, char **argv)
{
#ifdef CONFIG_PM
	struct sleep_stats st;
	bool allow;

	if (argc == 2 && !strcmp(argv[1], "reset")) {
		sleep_reset_stats();
		return 0;
	}
	if (argc == 2) {
		if (parse_on_off(sh, argv[1], &allow)) {
			return -EINVAL;
		}
		sleep_allow(allow);
		return 0;
	}

	sleep_get_stats(&st);
	shell_print(sh, "light sleep %s, USB power %s%s", st.allowed ? "allowed" : "off",
		    st.usb ? "present" : "absent", st.usb ? " (keeps it awake)" : "");
	shell_print(sh, "on battery %llu.%01llu s, %u.%01u %% of it in standby",
		    st.unlocked_ms / 1000, (st.unlocked_ms / 100) % 10,
		    (uint32_t)(st.unlocked_ms ? st.slept_ms * 1000 / st.unlocked_ms / 10 : 0),
		    (uint32_t)(st.unlocked_ms ? st.slept_ms * 1000 / st.unlocked_ms % 10 : 0));
	shell_print(sh, "%u entries, woken %u times by the timer, %u by a pin", st.entries,
		    st.timer_wakes, st.gpio_wakes);
	return 0;
#else
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_error(sh, "no CONFIG_PM");
	return -ENOTSUP;
#endif
}

static int cmd_ctrl_link(const struct shell *sh, size_t argc, char **argv)
{
	struct ctrl_link_state ls;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	ctrl_link_state(&ls);
	shell_print(sh, "network %08x, joined 0x%02x%s%s", ls.net_id, ls.joined_mask,
		    ls.scanning ? ", scanning" : "", ls.pairing ? ", pairing" : "");
	shell_print(sh, "channel %u%% busy, %u other network%s", ls.util_pct, ls.foreign_nets,
		    ls.foreign_nets == 1 ? "" : "s");
	shell_print(sh, "polls %u, held for other networks %u", ls.polls, ls.held);
	shell_print(sh, "csma: senses busy %u, frames deferred %u, sent anyway %u", ls.csma.busy,
		    ls.csma.deferred, ls.csma.forced);
	shell_print(sh, "rx ok %u, bad tag %u, replay %u, foreign %u, no session %u, malformed %u",
		    ls.stats.rx_ok, ls.stats.bad_tag, ls.stats.replay, ls.stats.foreign,
		    ls.stats.no_session, ls.stats.malformed);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(ctrl_cmds,
	SHELL_CMD(kbd, NULL, "Keyboard mode: drive the controls with single keys", cmd_ctrl_kbd),
	SHELL_CMD_ARG(select, NULL, "Toggle pad selection <1-48>", cmd_ctrl_select, 2, 0),
	SHELL_CMD_ARG(up, NULL, "D-pad up [times]", cmd_ctrl_up, 1, 1),
	SHELL_CMD_ARG(down, NULL, "D-pad down [times]", cmd_ctrl_down, 1, 1),
	SHELL_CMD_ARG(left, NULL, "D-pad left [times]", cmd_ctrl_left, 1, 1),
	SHELL_CMD_ARG(right, NULL, "D-pad right [times]", cmd_ctrl_right, 1, 1),
	SHELL_CMD(ok, NULL, "Press OK (toggle pad under cursor)", cmd_ctrl_ok),
	SHELL_CMD(hold, NULL, "Hold OK (settings, save pad setup)", cmd_ctrl_hold),
	SHELL_CMD(arm, NULL, "Press ARM", cmd_ctrl_arm),
	SHELL_CMD(fill, NULL, "Press FILL", cmd_ctrl_fill),
	SHELL_CMD(vent, NULL, "Press VENT", cmd_ctrl_vent),
	SHELL_CMD_ARG(fire, NULL, "Hold/release FIRE button <on|off>", cmd_ctrl_fire, 2, 0),
	SHELL_CMD(back, NULL, "Press BACK (disarm, back out)", cmd_ctrl_back),
	SHELL_CMD(back_hold, NULL, "Hold BACK (leave the button test)", cmd_ctrl_back_hold),
	SHELL_CMD(power, NULL, "Press the power button", cmd_ctrl_power),
	SHELL_CMD(link, NULL, "Link state and counters", cmd_ctrl_link),
	SHELL_CMD_ARG(battery, NULL, "Controller battery [<pct> <on|off>: fake it, no gauge only]",
		      cmd_ctrl_battery, 1, 2),
	SHELL_COND_CMD_ARG(CONFIG_PM, sleep, NULL,
			   "Light sleep statistics [on|off: allow it, reset: clear them]",
			   cmd_ctrl_sleep, 1, 1),
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

/* Satellite by letter (a-h) */
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
	SHELL_CMD_ARG(status, NULL, "Show simulated satellites [a-h]", cmd_sat_status, 1, 1),
	SHELL_CMD_ARG(link, NULL, "Radio link <a-h> <on|off>", cmd_sat_link, 3, 0),
	SHELL_CMD_ARG(loss, NULL, "Packet loss <a-h> <percent>", cmd_sat_loss, 3, 0),
	SHELL_CMD_ARG(key, NULL, "Pad key switch <a-h> <arm|safe>", cmd_sat_key, 3, 0),
	SHELL_CMD_ARG(res, NULL, "Channel load <a-h> <1-6> <mOhm|open>", cmd_sat_res, 4, 0),
	SHELL_CMD_ARG(vbat, NULL, "Pack voltage <a-h> <mV>", cmd_sat_vbat, 3, 0),
	SHELL_CMD_ARG(pair, NULL, "Pairing gesture: enter pairing mode <a-h>", cmd_sat_pair, 2, 0),
	SHELL_CMD_ARG(confirm, NULL, "Satellite user: LEDs match the controller <a-h>",
		      cmd_sat_confirm, 2, 0),
	SHELL_CMD_ARG(reject, NULL, "Satellite user: LEDs differ <a-h>", cmd_sat_confirm, 2, 0),
	SHELL_CMD_ARG(unpair, NULL, "Satellite forgets its controller <a-h>", cmd_sat_unpair, 2,
		      0),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(sat, &sat_cmds, "Simulated satellites", NULL);

#endif /* CONFIG_APP_SAT_SIM */
