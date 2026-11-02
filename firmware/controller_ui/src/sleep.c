/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Light sleep on the ESP32-S3: Zephyr PM's "standby" state.
 *
 * The kernel enters it by itself whenever every thread waits for longer
 * than the state's minimum residency, and the RTC timer wakes it for the
 * next timeout. The SX1262 keeps receiving meanwhile. This file adds what
 * the SoC port leaves to the board:
 *
 * - Pads keep their configuration while asleep. With CONFIG_PM the GPIO
 *   driver isolates every pad in sleep, which would float LCD_DISP, the
 *   antenna switch, the SPI lines and the keys' internal pull-ups.
 *
 * - Every pin with an edge interrupt wakes the chip: the keys, the power
 *   button, LORA_DIO1 and USB_PGOOD. The driver can only arm EXT1, which
 *   has one polarity for all pins and no IO41. GPIO wakeup is level-only,
 *   so for the time asleep each such pin waits for the level it is not at,
 *   with its interrupt masked. On the way out the edge interrupt comes
 *   back, and the change that woke the chip is still latched in the GPIO
 *   status, so the usual ISR sees it.
 *
 * - No sleep while USB power is present: the USB Serial/JTAG console does
 *   not survive it, and the battery is not being used anyway.
 *
 * Code that needs a peripheral clock to keep running while it waits (I2C,
 * the buzzer) holds pm_policy_state_all_lock_get() for that time.
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/policy.h>
#include <zephyr/spinlock.h>

#include <esp_sleep.h>
#include <hal/gpio_ll.h>
#include <soc/soc_caps.h>

#include "sleep.h"

LOG_MODULE_REGISTER(sleep, LOG_LEVEL_INF);

#define USER_NODE DT_PATH(zephyr_user)

static struct k_spinlock lock;
static struct sleep_stats stats = {.allowed = true};
static int64_t entered;          /* ticks, at the last standby entry */
static int64_t unlocked_since;   /* ticks, -1 while sleep is locked */

/* Pins armed for wakeup, with their edge interrupt to restore */
static uint64_t armed;
static uint8_t saved_type[SOC_GPIO_PIN_COUNT];
static uint8_t saved_ena[SOC_GPIO_PIN_COUNT];

static void arm_wake_pins(void)
{
	armed = 0;

	for (uint32_t n = 0; n < SOC_GPIO_PIN_COUNT; n++) {
		uint32_t type;
		uint32_t wake;
		bool high;

		if (!(SOC_GPIO_VALID_GPIO_MASK & BIT64(n)) || GPIO.pin[n].int_ena == 0) {
			continue;
		}

		/* Wake on the level the edge would lead to. A pin already
		 * there (DIO1 not yet served) waits for the next timeout.
		 */
		type = GPIO.pin[n].int_type;
		high = gpio_ll_get_level(&GPIO, n);

		if (type == GPIO_INTR_ANYEDGE ||
		    (type == GPIO_INTR_POSEDGE && !high) ||
		    (type == GPIO_INTR_NEGEDGE && high)) {
			wake = high ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL;
		} else {
			continue;
		}

		saved_type[n] = type;
		saved_ena[n] = GPIO.pin[n].int_ena;
		gpio_ll_intr_disable(&GPIO, n);
		gpio_ll_set_intr_type(&GPIO, n, wake);
		gpio_ll_wakeup_enable(&GPIO, n);
		armed |= BIT64(n);
	}
}

static void disarm_wake_pins(void)
{
	for (uint32_t n = 0; n < SOC_GPIO_PIN_COUNT; n++) {
		if (!(armed & BIT64(n))) {
			continue;
		}
		gpio_ll_wakeup_disable(&GPIO, n);
		gpio_ll_set_intr_type(&GPIO, n, saved_type[n]);
		/* A latched status fires the ISR right here */
		GPIO.pin[n].int_ena = saved_ena[n];
	}
	armed = 0;
}

/* Called with interrupts locked on the way in */
static void state_entry(enum pm_state state)
{
	if (state != PM_STATE_STANDBY) {
		return;
	}
	arm_wake_pins();
	entered = k_uptime_ticks();
}

/* Interrupts are already back on here, hence the masking above */
static void state_exit(enum pm_state state)
{
	if (state != PM_STATE_STANDBY) {
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);
	uint32_t causes = esp_sleep_get_wakeup_causes();

	disarm_wake_pins();
	stats.entries++;
	stats.slept_ms += k_ticks_to_ms_floor64(k_uptime_ticks() - entered);
	if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
		stats.timer_wakes++;
	}
	if (causes & BIT(ESP_SLEEP_WAKEUP_GPIO)) {
		stats.gpio_wakes++;
	}
	k_spin_unlock(&lock, key);
}

static struct pm_notifier notifier = {
	.state_entry = state_entry,
	.state_exit = state_exit,
};

/* Takes or drops the one PM lock for USB and the shell. Under lock. */
static void update_lock(bool was_locked)
{
	bool locked = stats.usb || !stats.allowed;
	int64_t now = k_uptime_ticks();

	if (locked == was_locked) {
		return;
	}
	if (locked) {
		pm_policy_state_all_lock_get();
		stats.unlocked_ms += k_ticks_to_ms_floor64(now - unlocked_since);
		unlocked_since = -1;
	} else {
		pm_policy_state_all_lock_put();
		unlocked_since = now;
	}
}

void sleep_allow(bool allow)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool was_locked = stats.usb || !stats.allowed;

	stats.allowed = allow;
	update_lock(was_locked);
	k_spin_unlock(&lock, key);
}

static void set_usb(bool usb)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool was_locked = stats.usb || !stats.allowed;

	stats.usb = usb;
	update_lock(was_locked);
	k_spin_unlock(&lock, key);
}

void sleep_get_stats(struct sleep_stats *out)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	*out = stats;
	if (unlocked_since >= 0) {
		out->unlocked_ms += k_ticks_to_ms_floor64(k_uptime_ticks() - unlocked_since);
	}
	k_spin_unlock(&lock, key);
}

void sleep_reset_stats(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int64_t now = k_uptime_ticks();

	stats.entries = 0;
	stats.timer_wakes = 0;
	stats.gpio_wakes = 0;
	stats.slept_ms = 0;
	stats.unlocked_ms = 0;
	if (unlocked_since >= 0) {
		unlocked_since = now;
	}
	k_spin_unlock(&lock, key);
}

#if DT_NODE_HAS_PROP(USER_NODE, usb_pgood_gpios)
static const struct gpio_dt_spec pgood = GPIO_DT_SPEC_GET(USER_NODE, usb_pgood_gpios);
static struct gpio_callback pgood_cb;

static void pgood_changed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	set_usb(gpio_pin_get_dt(&pgood) > 0);
}

/* USB_PGOOD's edge interrupt also makes it a wake pin */
static int watch_usb(void)
{
	int ret;

	if (!gpio_is_ready_dt(&pgood)) {
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&pgood, GPIO_INPUT);
	if (ret == 0) {
		gpio_init_callback(&pgood_cb, pgood_changed, BIT(pgood.pin));
		ret = gpio_add_callback_dt(&pgood, &pgood_cb);
	}
	if (ret == 0) {
		ret = gpio_pin_interrupt_configure_dt(&pgood, GPIO_INT_EDGE_BOTH);
	}
	if (ret == 0) {
		set_usb(gpio_pin_get_dt(&pgood) > 0);
	}
	return ret;
}
#else
static int watch_usb(void)
{
	return 0;
}
#endif

static int sleep_init(void)
{
	int ret;

	/* Locked until we know whether USB is there */
	pm_policy_state_all_lock_get();
	unlocked_since = -1;
	stats.usb = true;

	/* After the GPIO driver's POST_KERNEL init, which turned it on */
	esp_sleep_enable_gpio_switch(false);
	esp_sleep_enable_gpio_wakeup();
	pm_notifier_register(&notifier);

	ret = watch_usb();
	if (ret) {
		/* Without the line, assume USB, and stay awake */
		LOG_ERR("no USB power detection (%d), light sleep off", ret);
		return ret;
	}
	return 0;
}

SYS_INIT(sleep_init, APPLICATION, 2);
