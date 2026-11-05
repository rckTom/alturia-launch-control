/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * native_sim: the satellite's LEDs and pairing button in the radio network
 * simulator's web UI, and an igniter on every output for the continuity ADC. The LEDs are emulated GPIOs (boards/native_sim.overlay)
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
	radio_sim_set_button_cb(on_button);
	k_work_reschedule(&poll_work, K_MSEC(POLL_MS));
	return 0;
}

SYS_INIT(sim_io_init, APPLICATION, 90);
