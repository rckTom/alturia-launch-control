/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The satellite's half of pairing, see design/radio_security.md.
 *
 * With every channel SAFE, holding the button for PAIR_HOLD_MS enters
 * pairing mode; the status LED blinks fast. Once the console has found the
 * satellite, both compute the same 24-bit pattern. The six channel LEDs
 * show it as four frames in turn, with the status LED lit on the first,
 * while the console draws the same frames as four rows. A short press
 * confirms that they match, a long press rejects.
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

#define PAIR_HOLD_MS   5000
#define REJECT_HOLD_MS 2000

#define TICK_MS      125
#define FRAME_TICKS  12   /* 1.5 s per frame */
#define GAP_TICKS    4    /* 0.5 s dark before the sequence repeats */

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
	struct sat_link_state ls;
	uint32_t period = LSEC_SAS_FRAMES * FRAME_TICKS + GAP_TICKS;

	sat_link_state(&ls);
	ticks++;

	if (!ls.pairing) {
		sas_shown = false;
		set_leds(0, false);
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

static void on_button(struct input_event *evt, void *user_data)
{
	static int64_t down_at;
	int64_t held;

	if (evt->type != INPUT_EV_KEY || evt->code != INPUT_KEY_0) {
		return;
	}

	if (evt->value) {
		down_at = k_uptime_get();
		return;
	}

	held = k_uptime_get() - down_at;

	if (sas_shown) {
		sat_link_pair_confirm(held < REJECT_HOLD_MS);
	} else if (held >= PAIR_HOLD_MS) {
		pair_ui_start();
	}
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

	return 0;
}
