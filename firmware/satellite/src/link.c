/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/logging/log.h>

#include "satellite.h"

LOG_MODULE_REGISTER(link, LOG_LEVEL_INF);

static const struct device *const radio = DEVICE_DT_GET(DT_ALIAS(lora0));

int link_init(void)
{
	struct lora_modem_config cfg = {
		.frequency = 868300000,
		.bandwidth = BW_125_KHZ,
		.datarate = SF_9,
		.preamble_len = 8,
		.coding_rate = CR_4_5,
		.iq_inverted = false,
		.public_network = false,
		.tx_power = 14,
		.tx = false,
	};

	if (!device_is_ready(radio)) {
		return -ENODEV;
	}

	return lora_config(radio, &cfg);
}

int link_send(const uint8_t *buf, size_t len)
{
	return lora_send(radio, (uint8_t *)buf, len);
}

int link_recv(uint8_t *buf, size_t len, int32_t timeout_ms)
{
	int16_t rssi;
	int8_t snr;

	return lora_recv(radio, buf, len, K_MSEC(timeout_ms), &rssi, &snr);
}
