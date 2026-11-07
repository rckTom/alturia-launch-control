/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * native_sim: the satellite's LEDs and pairing button in the radio network
 * simulator's web UI, an igniter on every output for the continuity ADC,
 * and its current for the ignition log. The LEDs are emulated GPIOs (boards/native_sim.overlay)
 * whose levels go to the simulator as a bitmask; its button press lands on
 * the button's pin, so pair_ui.c sees it through gpio-keys as on the board.
 *
 * LED mask: bits 0-5 channel LEDs 1-6, bit 6 status LED, bits 8-13 the
 * high-side enables of channels 1-6 (armed).
 */

#include <zephyr/device.h>
#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include "radio_sim.h"

#define PIN(node) DT_GPIO_PIN(node, gpios)

#define POLL_MS 50

static const struct device *const gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device *const adc_cont = DEVICE_DT_GET(DT_NODELABEL(adc_continuity));
static const struct device *const adc_rails = DEVICE_DT_GET(DT_NODELABEL(adc_rails));

/* What the continuity ADC reads on every output: an igniter of about
 * 1.5 Ohm at commands.c's provisional 10 mOhm per mV.
 */
#define IGNITER_MV 150

static const gpio_pin_t led_pins[] = {
	PIN(DT_ALIAS(chan_led0)), PIN(DT_ALIAS(chan_led1)), PIN(DT_ALIAS(chan_led2)),
	PIN(DT_ALIAS(chan_led3)), PIN(DT_ALIAS(chan_led4)), PIN(DT_ALIAS(chan_led5)),
	PIN(DT_ALIAS(led0)),
};

static const gpio_pin_t armed_pins[] = {
	PIN(DT_NODELABEL(en_h1)), PIN(DT_NODELABEL(en_h2)), PIN(DT_NODELABEL(en_h3)),
	PIN(DT_NODELABEL(en_h4)), PIN(DT_NODELABEL(en_h5)), PIN(DT_NODELABEL(en_h6)),
};

static const gpio_pin_t low_pins[] = {
	PIN(DT_NODELABEL(en_l1)), PIN(DT_NODELABEL(en_l2)), PIN(DT_NODELABEL(en_l3)),
	PIN(DT_NODELABEL(en_l4)), PIN(DT_NODELABEL(en_l5)), PIN(DT_NODELABEL(en_l6)),
};

/*
 * The ignition log's view of an igniter (satellite/src/ignlog.c): while a
 * channel's switches are both on, its TPS1HB08 sense reads the current
 * through R_SNS, 560 Ohm at 1/5000 of it. Channels 1-5 carry an e-match
 * of about 1.8 A that opens after 5, 8, 11, 14 and 17 ms; channel 6 a
 * bridge that never opens, at 3.2 A. V_PYRO (CH6) sags with the current.
 */
#define SENSE_MV(ma)     ((ma) * 560 / 5000)
#define VPYRO_TAP_MV     2581   /* 11.1 V over 4.3 */
#define SIM_RAIL_CHANNELS 6

static int64_t on_since[SIM_RAIL_CHANNELS];
static int32_t flowing_ma[SIM_RAIL_CHANNELS];

static int32_t channel_ma(int ch)
{
	bool on = gpio_emul_output_get(gpio0, armed_pins[ch]) > 0 &&
		  gpio_emul_output_get(gpio0, low_pins[ch]) > 0;
	int64_t now = k_uptime_ticks();
	int64_t us;

	if (!on) {
		on_since[ch] = 0;
		return 0;
	}
	if (on_since[ch] == 0) {
		on_since[ch] = now;
	}
	us = k_ticks_to_us_floor64(now - on_since[ch]);

	if (ch == SIM_RAIL_CHANNELS - 1) {
		return us < 300 ? 3200 * us / 300 : 3200 + (int32_t)(us / 1000 % 3) * 20;
	}
	if (us < 300) {
		return 1800 * us / 300;                    /* the sense output settling */
	}
	if (us < (5 + 3 * ch) * 1000) {
		return 1800 - (int32_t)(us / 1000) * 15;   /* the bridge heating up */
	}
	return 0;                                          /* burnt through */
}

/* MCP3208 CH(5 - ch) is channel ch; CH6 is V_PYRO. */
static int rails_value(const struct device *dev, unsigned int chan, void *data,
		       uint32_t *result)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(data);

	if (chan < SIM_RAIL_CHANNELS) {
		int ch = SIM_RAIL_CHANNELS - 1 - chan;

		flowing_ma[ch] = channel_ma(ch);
		*result = SENSE_MV(flowing_ma[ch]);
	} else {
		int32_t total = 0;

		for (int ch = 0; ch < SIM_RAIL_CHANNELS; ch++) {
			total += flowing_ma[ch];
		}
		*result = VPYRO_TAP_MV - total * 35 / 1000;  /* 0.15 V per A at the rail */
	}
	return 0;
}

static void poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(poll_work, poll_fn);

static void poll_fn(struct k_work *work)
{
	uint16_t mask = 0;

	for (int i = 0; i < ARRAY_SIZE(led_pins); i++) {
		if (gpio_emul_output_get(gpio0, led_pins[i]) > 0) {
			mask |= BIT(i);
		}
	}
	for (int i = 0; i < ARRAY_SIZE(armed_pins); i++) {
		if (gpio_emul_output_get(gpio0, armed_pins[i]) > 0) {
			mask |= BIT(8 + i);
		}
	}

	radio_sim_report_leds(mask);
	k_work_reschedule(&poll_work, K_MSEC(POLL_MS));
}

static void on_button(uint8_t index, bool pressed)
{
	if (index == 0) {
		gpio_emul_input_set(gpio0, PIN(DT_NODELABEL(pair_button)), pressed);
	}
}

static int sim_io_init(void)
{
	for (int ch = 0; ch < 6; ch++) {
		adc_emul_const_value_set(adc_cont, ch, IGNITER_MV);
	}
	for (int chan = 0; chan <= SIM_RAIL_CHANNELS; chan++) {
		adc_emul_value_func_set(adc_rails, chan, rails_value, NULL);
	}
	radio_sim_set_button_cb(on_button);
	k_work_reschedule(&poll_work, K_MSEC(POLL_MS));
	return 0;
}

SYS_INIT(sim_io_init, APPLICATION, 90);
