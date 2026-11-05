/* SPDX-License-Identifier: GPL-3.0-only */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include "satellite.h"

LOG_MODULE_REGISTER(rail, LOG_LEVEL_INF);

/* The rail voltages the board can measure: voltage-divider sensors on the
 * MCP3208 and VDDA from the STM32's VREFINT, where the devicetree has them.
 */
#define RAIL_SENSE(label, rail_name)                                                   \
	IF_ENABLED(DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(label)),                        \
		   ({.name = rail_name, .dev = DEVICE_DT_GET(DT_NODELABEL(label))},))

static const struct {
	const char *name;
	const struct device *dev;
} rails[] = {
	RAIL_SENSE(vbat_sense, "VBAT_FUSED")
	RAIL_SENSE(vpyro_sense, "V_PYRO_ARMED")
	RAIL_SENSE(vref, "+3V3 (VDDA)")
};

size_t rails_count(void)
{
	return ARRAY_SIZE(rails);
}

int rails_read(size_t i, const char **name, int32_t *mv)
{
	struct sensor_value v;
	int ret;

	if (i >= ARRAY_SIZE(rails)) {
		return -ENOENT;
	}

	*name = rails[i].name;
	if (!device_is_ready(rails[i].dev)) {
		return -ENODEV;
	}

	ret = sensor_sample_fetch_chan(rails[i].dev, SENSOR_CHAN_VOLTAGE);
	if (ret) {
		return ret;
	}

	ret = sensor_channel_get(rails[i].dev, SENSOR_CHAN_VOLTAGE, &v);
	if (ret) {
		return ret;
	}

	*mv = (int32_t)sensor_value_to_milli(&v);
	return 0;
}

#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(vbat_sense))

int rail_vbat_mv(int32_t *mv)
{
	const struct device *const dev = DEVICE_DT_GET(DT_NODELABEL(vbat_sense));
	struct sensor_value v;
	int ret;

	if (!device_is_ready(dev)) {
		return -ENODEV;
	}

	ret = sensor_sample_fetch_chan(dev, SENSOR_CHAN_VOLTAGE);
	if (!ret) {
		ret = sensor_channel_get(dev, SENSOR_CHAN_VOLTAGE, &v);
	}
	if (!ret) {
		*mv = (int32_t)sensor_value_to_milli(&v);
	}
	return ret;
}

#elif defined(CONFIG_BOARD_NATIVE_SIM)

/* The radio network simulator's satellites run on a charged pack. */
int rail_vbat_mv(int32_t *mv)
{
	*mv = 12000;
	return 0;
}

#else

int rail_vbat_mv(int32_t *mv)
{
	return -ENOTSUP;
}

#endif
