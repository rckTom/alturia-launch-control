/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * The satellite's button and LEDs: the continuity display, and the
 * satellite's half of pairing (documentation/radio_link.md).
 *
 * A press shows continuity on the channel LEDs for CONT_SHOW_MS: steady for
 * an output that reads OK, fast blinking for an open one, slow blinking for
 * one that reads shorted or could not be measured. The readings are the main loop's,
 * which goes round all outputs every 4 s or so, so the LEDs follow them.
 *
 * With every channel SAFE, PAIR_PRESSES presses within PAIR_WINDOW_MS enter
 * pairing mode; the status LED blinks fast. Once the console has found the
 * satellite, both compute the same 24-bit pattern. The six channel LEDs
 * show it as four frames in turn, with the status LED lit on the first,
 * while the console draws the same frames as four rows. A short press
 * confirms that they match, a long press rejects.
 *
 * Otherwise the status LED says the satellite is on: a short blink every
 * 2 s, lit for as long as a session with the controller is up.
 *
 * The LEDs are the devicetree aliases chan-led0..chan-led5 and led0. A
 * board without the channel LEDs (the Nucleo) gets the pattern in the log.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "lsec.h"
#include "sat_link.h"
#include "satellite.h"

LOG_MODULE_REGISTER(pair_ui, LOG_LEVEL_INF);

#define PAIR_PRESSES   3
#define PAIR_WINDOW_MS 1000
#define REJECT_HOLD_MS 2000
#define CONT_SHOW_MS   5000

#define TICK_MS      125
#define FRAME_TICKS  12   /* 1.5 s per frame */
#define GAP_TICKS    4    /* 0.5 s dark before the sequence repeats */
#define STATUS_TICKS 16   /* the status LED's blink while on, every 2 s */

#define HAVE_CHAN_LEDS DT_NODE_EXISTS(DT_ALIAS(chan_led5))

#if HAVE_CHAN_LEDS
#define CHAN_LED(i) GPIO_DT_SPEC_GET(DT_ALIAS(chan_led##i), gpios)
static const struct gpio_dt_spec chan_led[LSEC_SAS_LEDS] = {
	CHAN_LED(0), CHAN_LED(1), CHAN_LED(2), CHAN_LED(3), CHAN_LED(4), CHAN_LED(5),
};
#endif

static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});

static uint8_t sas[LSEC_SAS_FRAMES];
static bool sas_shown;
static uint32_t ticks;
/* k_uptime_get_32() until which the LEDs show continuity. */
static atomic_t cont_until;

static void tick_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tick_work, tick_fn);

static void set_leds(uint8_t pattern, bool status)
{
#if HAVE_CHAN_LEDS
	for (int i = 0; i < LSEC_SAS_LEDS; i++) {
		gpio_pin_set_dt(&chan_led[i], pattern & BIT(i));
	}
#endif
	if (status_led.port) {
		gpio_pin_set_dt(&status_led, status);
	}
}

static void tick_fn(struct k_work *work)
{
	uint32_t period = LSEC_SAS_FRAMES * FRAME_TICKS + GAP_TICKS;

	ticks++;

	if (!sat_link_pairing()) {
		uint8_t chan = 0;

		sas_shown = false;
		if ((int32_t)((uint32_t)atomic_get(&cont_until) - k_uptime_get_32()) > 0) {
			uint8_t ok, bad;

			commands_cont_masks(&ok, &bad);
			uint8_t open = ~(ok | bad) & BIT_MASK(LSEC_SAS_LEDS);

			chan = ok | ((ticks & 1) ? open : 0) | ((ticks / 4) & 1 ? bad : 0);
		}
		/* Powered: a blink every STATUS_TICKS; with the controller: lit. */
		set_leds(chan, sat_link_joined() || ticks % STATUS_TICKS == 0);
		k_work_reschedule(&tick_work, K_MSEC(TICK_MS));
		return;
	}

	if (sas_shown) {
		uint32_t t = ticks % period;
		int frame = t / FRAME_TICKS;

		if (frame < LSEC_SAS_FRAMES) {
			set_leds(sas[frame], frame == 0);
		} else {
			set_leds(0, false);
		}
	} else {
		/* Pairing mode, waiting for the console: fast blink. */
		set_leds(0, ticks & 1);
	}

	k_work_reschedule(&tick_work, K_MSEC(TICK_MS));
}

int pair_ui_start(void)
{
	int ret;

	if (!channels_all_safe()) {
		LOG_WRN("pairing refused: channels armed");
		return -EBUSY;
	}

	ret = sat_link_pair_start();
	if (ret) {
		return ret;
	}

	LOG_INF("pairing mode");
	sas_shown = false;
	k_work_reschedule(&tick_work, K_NO_WAIT);
	return 0;
}

void pair_ui_show(const uint8_t *pattern)
{
	if (!pattern) {
		sas_shown = false;
		return;
	}

	memcpy(sas, pattern, sizeof(sas));
	ticks = 0;
	sas_shown = true;
	k_work_reschedule(&tick_work, K_NO_WAIT);

	if (!HAVE_CHAN_LEDS) {
		char rows[LSEC_SAS_FRAMES][LSEC_SAS_LEDS + 1];

		for (int r = 0; r < LSEC_SAS_FRAMES; r++) {
			for (int i = 0; i < LSEC_SAS_LEDS; i++) {
				rows[r][i] = (sas[r] & BIT(i)) ? '#' : '.';
			}
			rows[r][LSEC_SAS_LEDS] = '\0';
		}
		LOG_INF("compare: %s %s %s %s", rows[0], rows[1], rows[2], rows[3]);
	}

	LOG_INF("press the button if the console shows the same, hold it if not");
}

static void show_continuity(void)
{
	atomic_set(&cont_until, (atomic_val_t)(k_uptime_get_32() + CONT_SHOW_MS));
	k_work_reschedule(&tick_work, K_NO_WAIT);
}

static void on_button(struct input_event *evt, void *user_data)
{
	static int64_t down_at;
	static int64_t presses_from;
	static int presses;
	int64_t now = k_uptime_get();

	if (evt->type != INPUT_EV_KEY || evt->code != INPUT_KEY_0) {
		return;
	}

	if (evt->value) {
		down_at = now;
		return;
	}

	if (sas_shown) {
		sat_link_pair_confirm(now - down_at < REJECT_HOLD_MS);
		return;
	}
	if (sat_link_pairing()) {
		return;   /* waiting for the console; the LEDs are pairing's */
	}

	if (presses == 0 || now - presses_from > PAIR_WINDOW_MS) {
		presses = 0;
		presses_from = down_at;
	}
	if (++presses >= PAIR_PRESSES) {
		presses = 0;
		pair_ui_start();
		return;
	}

	show_continuity();
}
INPUT_CALLBACK_DEFINE(NULL, on_button, NULL);

int pair_ui_init(void)
{
	int ret;

#if HAVE_CHAN_LEDS
	for (int i = 0; i < LSEC_SAS_LEDS; i++) {
		ret = gpio_pin_configure_dt(&chan_led[i], GPIO_OUTPUT_INACTIVE);
		if (ret) {
			return ret;
		}
	}
#endif
	if (status_led.port) {
		ret = gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_INACTIVE);
		if (ret) {
			return ret;
		}
	}

	k_work_reschedule(&tick_work, K_NO_WAIT);
	return 0;
}
