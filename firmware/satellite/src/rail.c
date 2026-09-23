/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include "satellite.h"

LOG_MODULE_REGISTER(rail, LOG_LEVEL_INF);

/* INA226 across the 2 mOhm shunt between the arm relay and the fire rail.
 * A few mA of unexpected current with no channel firing means a leaking
 * output, which is the fault this monitor exists to catch.
 */
static const struct device *const monitor = DEVICE_DT_GET(DT_NODELABEL(rail_monitor));

int rail_init(void)
{
	return device_is_ready(monitor) ? 0 : -ENODEV;
}

int rail_read(int32_t *mv, int32_t *ma)
{
	struct sensor_value voltage;
	struct sensor_value current;
	int ret;

	ret = sensor_sample_fetch(monitor);
	if (ret) {
		return ret;
	}

	ret = sensor_channel_get(monitor, SENSOR_CHAN_VOLTAGE, &voltage);
	if (ret) {
		return ret;
	}

	ret = sensor_channel_get(monitor, SENSOR_CHAN_CURRENT, &current);
	if (ret) {
		return ret;
	}

	*mv = voltage.val1 * 1000 + voltage.val2 / 1000;
	*ma = current.val1 * 1000 + current.val2 / 1000;

	return 0;
}
