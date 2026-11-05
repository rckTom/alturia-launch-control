/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#ifdef CONFIG_ARCH_POSIX
#include <posix_board_if.h>
#endif

#include "inputs.h"
#include "power.h"

LOG_MODULE_REGISTER(power, LOG_LEVEL_INF);

#define USER_NODE DT_PATH(zephyr_user)

#define HAVE_LATCH DT_NODE_HAS_PROP(USER_NODE, pwr_off_gpios)

/* Long enough to read the shutdown splash */
#define OFF_DELAY_MS 1500

#if HAVE_LATCH
/* PWR_OFF high pulls the latch's K node low through Q103 */
static const struct gpio_dt_spec pwr_off = GPIO_DT_SPEC_GET(USER_NODE, pwr_off_gpios);

static int power_init(void)
{
	if (!gpio_is_ready_dt(&pwr_off)) {
		return -ENODEV;
	}
	return gpio_pin_configure_dt(&pwr_off, GPIO_OUTPUT_INACTIVE);
}

SYS_INIT(power_init, APPLICATION, 0);
#endif

static void off_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(off_work, off_work_fn);

static void off_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (inputs_pressed(BTN_POWER)) {
		k_work_reschedule(&off_work, K_MSEC(20));
		return;
	}
	LOG_INF("power off");
#if HAVE_LATCH
	(void)gpio_pin_set_dt(&pwr_off, 1);
#elif defined(CONFIG_ARCH_POSIX)
	/* native_sim: the process exits */
	posix_exit(0);
#endif
}

bool power_can_off(void)
{
#if HAVE_LATCH
	return gpio_is_ready_dt(&pwr_off);
#else
	return IS_ENABLED(CONFIG_ARCH_POSIX);
#endif
}

void power_off(void)
{
	k_work_reschedule(&off_work, K_MSEC(OFF_DELAY_MS));
}
