/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The controller's physical controls, driven from the radio network
 * simulator's web UI. Keys land on the gpio0 pins that boards/
 * native_sim.overlay gives the gpio-keys, so they take the same path as
 * the SDL keyboard (and the hardware buttons); the encoder is reported as
 * the mouse wheel and click were. inputs.c listens to every input device.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

#include "radio_sim.h"

LOG_MODULE_REGISTER(sim_panel, LOG_LEVEL_INF);

static const struct device *const gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

static void on_input(enum rsim_input_type type, uint16_t code, int32_t value)
{
	switch (type) {
	case RSIM_IN_PIN:
		gpio_emul_input_set(gpio0, code, value ? 1 : 0);
		break;
	case RSIM_IN_WHEEL:
		input_report_rel(NULL, INPUT_REL_WHEEL, value, true, K_FOREVER);
		break;
	case RSIM_IN_TOUCH:
		input_report_key(NULL, INPUT_BTN_TOUCH, value ? 1 : 0, true, K_FOREVER);
		break;
	default:
		break;
	}
}

static int sim_panel_init(void)
{
	radio_sim_set_input_cb(on_input);
	return 0;
}

SYS_INIT(sim_panel_init, APPLICATION, 90);
