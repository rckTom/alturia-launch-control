/* SPDX-License-Identifier: GPL-3.0-only */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "satellite.h"

LOG_MODULE_REGISTER(arm, LOG_LEVEL_INF);

/*
 * The arm relay K1 (hardware/satellite 04_arm). Its coil is fed from VKEY,
 * so the key relay K2 must be closed, and switched by Q2 from the '123
 * one-shot U6: Q stays high for about 150 ms (150k x 1u) after each falling
 * edge on ARM_PUMP, as long as ARM_ENABLE (CLR) is high. Dropping
 * ARM_ENABLE releases K1 at once.
 *
 * The pump is toggled from a work item, in software, so K1 falls out
 * when the firmware stops running; never from a timer peripheral or PWM.
 */
#if DT_NODE_EXISTS(DT_NODELABEL(arm_enable)) && DT_NODE_EXISTS(DT_NODELABEL(arm_pump))

/* Half period: a falling edge every 80 ms against the 150 ms one-shot. */
#define PUMP_HALF_MS 40

static const struct gpio_dt_spec arm_enable = GPIO_DT_SPEC_GET(DT_NODELABEL(arm_enable), gpios);
static const struct gpio_dt_spec arm_pump = GPIO_DT_SPEC_GET(DT_NODELABEL(arm_pump), gpios);
#if DT_NODE_EXISTS(DT_NODELABEL(key_enabled))
static const struct gpio_dt_spec key = GPIO_DT_SPEC_GET(DT_NODELABEL(key_enabled), gpios);
#endif

static bool pumping;
static bool pump_level;

static void pump_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(pump_work, pump_fn);

static void pump_fn(struct k_work *work)
{
	if (!pumping) {
		return;
	}

	pump_level = !pump_level;
	(void)gpio_pin_set_dt(&arm_pump, pump_level);
	k_work_reschedule(&pump_work, K_MSEC(PUMP_HALF_MS));
}

int arm_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&arm_enable) || !gpio_is_ready_dt(&arm_pump)) {
		return -ENODEV;
	}

	/* CLR first, so that nothing below can fire the one-shot. */
	ret = gpio_pin_configure_dt(&arm_enable, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		return ret;
	}

	return gpio_pin_configure_dt(&arm_pump, GPIO_OUTPUT_INACTIVE);
}

int arm_relay_set(bool on)
{
	int ret;

	if (!on) {
		pumping = false;
		k_work_cancel_delayable(&pump_work);
		ret = gpio_pin_set_dt(&arm_enable, 0);
		(void)gpio_pin_set_dt(&arm_pump, 0);
		pump_level = false;
		LOG_INF("arm relay released");
		return ret;
	}

	if (pumping) {
		return 0;
	}

	/* ARM_PUMP high before CLR rises: CLR rising with A low triggers
	 * the one-shot by itself. Then the falling edges hold it.
	 */
	pump_level = true;
	ret = gpio_pin_set_dt(&arm_pump, 1);
	if (ret) {
		return ret;
	}

	ret = gpio_pin_set_dt(&arm_enable, 1);
	if (ret) {
		(void)gpio_pin_set_dt(&arm_pump, 0);
		return ret;
	}

	pumping = true;
	k_work_reschedule(&pump_work, K_MSEC(PUMP_HALF_MS));
	LOG_INF("arm relay pumping");
	return 0;
}

bool arm_relay_on(void)
{
	return pumping;
}

int arm_key_on(void)
{
#if DT_NODE_EXISTS(DT_NODELABEL(key_enabled))
	return gpio_pin_get_dt(&key);
#else
	return -ENOTSUP;
#endif
}

#else /* No arm path in the devicetree (native_sim, the Nucleo). */

int arm_init(void)
{
	return 0;
}

int arm_relay_set(bool on)
{
	return on ? -ENOTSUP : 0;
}

bool arm_relay_on(void)
{
	return false;
}

int arm_key_on(void)
{
	return -ENOTSUP;
}

#endif
