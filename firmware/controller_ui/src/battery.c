/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/charger.h>
#include <zephyr/drivers/fuel_gauge.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "battery.h"

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

#define POLL_MS 2000

#define GAUGE_NODE   DT_ALIAS(fuel_gauge0)
#define CHARGER_NODE DT_ALIAS(charger0)
#define USER_NODE    DT_PATH(zephyr_user)

#define HAVE_GAUGE   (IS_ENABLED(CONFIG_FUEL_GAUGE) && DT_NODE_HAS_STATUS_OKAY(GAUGE_NODE))
#define HAVE_CHARGER (IS_ENABLED(CONFIG_CHARGER) && DT_NODE_HAS_STATUS_OKAY(CHARGER_NODE))

static K_MUTEX_DEFINE(lock);
static struct battery_info info = {.pgood = -1};

#if DT_NODE_HAS_PROP(USER_NODE, usb_pgood_gpios)
static const struct gpio_dt_spec pgood = GPIO_DT_SPEC_GET(USER_NODE, usb_pgood_gpios);
#endif

#if HAVE_GAUGE
static void read_gauge(struct battery_info *b)
{
	const struct device *dev = DEVICE_DT_GET(GAUGE_NODE);
	fuel_gauge_prop_t props[] = {
		FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE_PCT,
		FUEL_GAUGE_VOLTAGE_UV,
		FUEL_GAUGE_RUNTIME_TO_EMPTY_MINS,
		FUEL_GAUGE_RUNTIME_TO_FULL_MINS,
	};
	union fuel_gauge_prop_val vals[ARRAY_SIZE(props)];

	b->gauge = device_is_ready(dev) &&
		   fuel_gauge_get_props(dev, props, vals, ARRAY_SIZE(props)) == 0;
	if (!b->gauge) {
		return;
	}
	b->soc_pct = MIN(vals[0].relative_state_of_charge_pct, 100);
	b->voltage_mv = vals[1].voltage_uv / 1000;
	b->to_empty_min = vals[2].runtime_to_empty_mins;
	b->to_full_min = vals[3].runtime_to_full_mins;
}
#endif

#if HAVE_CHARGER
static uint32_t charger_u32(const struct device *dev, charger_prop_t prop, bool *ok)
{
	union charger_propval val;

	if (charger_get_prop(dev, prop, &val)) {
		*ok = false;
		return 0;
	}
	/* Every property read here is a 32-bit current or voltage */
	return val.const_charge_current_ua;
}

static void read_charger(struct battery_info *b)
{
	const struct device *dev = DEVICE_DT_GET(CHARGER_NODE);
	union charger_propval val;
	bool ok = device_is_ready(dev);

	if (!ok) {
		b->charger = false;
		return;
	}
	ok &= charger_get_prop(dev, CHARGER_PROP_ONLINE, &val) == 0;
	b->online = val.online != CHARGER_ONLINE_OFFLINE;
	ok &= charger_get_prop(dev, CHARGER_PROP_STATUS, &val) == 0;
	b->status = val.status;
	ok &= charger_get_prop(dev, CHARGER_PROP_HEALTH, &val) == 0;
	b->health = val.health;

	b->ichg_ma = charger_u32(dev, CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA, &ok) / 1000;
	b->vreg_mv = charger_u32(dev, CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV, &ok) / 1000;
	b->iinlim_ma = charger_u32(dev, CHARGER_PROP_INPUT_REGULATION_CURRENT_UA, &ok) / 1000;
	b->vindpm_mv = charger_u32(dev, CHARGER_PROP_INPUT_REGULATION_VOLTAGE_UV, &ok) / 1000;
	b->iprechg_ma = charger_u32(dev, CHARGER_PROP_PRECHARGE_CURRENT_UA, &ok) / 1000;
	b->iterm_ma = charger_u32(dev, CHARGER_PROP_CHARGE_TERM_CURRENT_UA, &ok) / 1000;
	b->charger = ok;
}
#endif

static void poll(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(poll_work, poll);

static void poll(struct k_work *work)
{
	struct battery_info b;

	ARG_UNUSED(work);

	k_mutex_lock(&lock, K_FOREVER);
	b = info;
	k_mutex_unlock(&lock);

#if HAVE_GAUGE
	read_gauge(&b);
#endif
#if HAVE_CHARGER
	read_charger(&b);
#endif
#if DT_NODE_HAS_PROP(USER_NODE, usb_pgood_gpios)
	b.pgood = gpio_pin_get_dt(&pgood);
	b.pgood = b.pgood < 0 ? -1 : b.pgood;
#endif

	k_mutex_lock(&lock, K_FOREVER);
	info = b;
	k_mutex_unlock(&lock);

	k_work_reschedule(&poll_work, K_MSEC(POLL_MS));
}

static int battery_init(void)
{
#if DT_NODE_HAS_PROP(USER_NODE, usb_pgood_gpios)
	if (gpio_is_ready_dt(&pgood)) {
		(void)gpio_pin_configure_dt(&pgood, GPIO_INPUT);
	}
#endif
	k_work_reschedule(&poll_work, K_NO_WAIT);
	return 0;
}

SYS_INIT(battery_init, APPLICATION, 1);

void battery_get(struct battery_info *out)
{
	k_mutex_lock(&lock, K_FOREVER);
	*out = info;
	k_mutex_unlock(&lock);
}

bool battery_charging(const struct battery_info *b)
{
	return b->charger && b->status == CHARGER_STATUS_CHARGING;
}

int battery_fake(uint8_t soc_pct, bool charging)
{
	if (HAVE_GAUGE) {
		return -EBUSY;
	}
	k_mutex_lock(&lock, K_FOREVER);
	info.gauge = true;
	info.soc_pct = MIN(soc_pct, 100);
	info.voltage_mv = 3300 + 9 * info.soc_pct;
	info.charger = true;
	info.online = charging;
	info.status = charging ? CHARGER_STATUS_CHARGING : CHARGER_STATUS_DISCHARGING;
	info.health = CHARGER_HEALTH_GOOD;
	k_mutex_unlock(&lock);
	return 0;
}

const char *battery_status_text(enum charger_status status)
{
	switch (status) {
	case CHARGER_STATUS_CHARGING:
		return "charging";
	case CHARGER_STATUS_DISCHARGING:
		return "discharging";
	case CHARGER_STATUS_NOT_CHARGING:
		return "not charging";
	case CHARGER_STATUS_FULL:
		return "full";
	default:
		return "unknown";
	}
}

const char *battery_health_text(enum charger_health health)
{
	switch (health) {
	case CHARGER_HEALTH_GOOD:
		return "good";
	case CHARGER_HEALTH_OVERHEAT:
		return "overheat";
	case CHARGER_HEALTH_OVERVOLTAGE:
		return "overvoltage";
	case CHARGER_HEALTH_UNSPEC_FAILURE:
		return "input fault";
	case CHARGER_HEALTH_COLD:
		return "too cold";
	case CHARGER_HEALTH_WATCHDOG_TIMER_EXPIRE:
		return "watchdog";
	case CHARGER_HEALTH_SAFETY_TIMER_EXPIRE:
		return "timer expired";
	case CHARGER_HEALTH_HOT:
		return "too hot";
	default:
		return "unknown";
	}
}
