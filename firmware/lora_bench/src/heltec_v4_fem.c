/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * RF front-end of the Heltec WiFi LoRa 32 V4.
 *
 * The V4 puts a PA/LNA between the SX1262 and the antenna, and the part
 * changed between board revisions:
 *
 *   V4.2  GC1109    CTX = SX1262 DIO2, CPS = GPIO46 (high: PA, low: bypass)
 *   V4.3  KCT8103L  CPS = SX1262 DIO2, CTX = GPIO5  (high: TX, low: RX LNA)
 *
 * On both, GPIO7 enables the front-end's LDO and GPIO2 drives its CSD
 * (chip enable). DIO2 is switched by the SX1262 itself; the one pin left
 * to software must be high while transmitting and low otherwise. Only
 * which pin that is differs between revisions.
 *
 * The revisions are told apart by what they put on CSD: V4.3 pulls it up,
 * V4.2 pulls it down. This is the detection Meshtastic uses.
 *
 * Pin assignments are from Heltec's schematics WiFi_LoRa_32_V4.2.pdf and
 * HTIT-WB32LAF_V4.3.pdf.
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "heltec_v4_fem.h"

LOG_MODULE_REGISTER(fem, LOG_LEVEL_INF);

#define USER_NODE DT_PATH(zephyr_user)

static const struct gpio_dt_spec power = GPIO_DT_SPEC_GET(USER_NODE, fem_power_gpios);
static const struct gpio_dt_spec csd = GPIO_DT_SPEC_GET(USER_NODE, fem_csd_gpios);
static const struct gpio_dt_spec gc1109_cps = GPIO_DT_SPEC_GET(USER_NODE, fem_gc1109_cps_gpios);
static const struct gpio_dt_spec kct8103l_ctx =
	GPIO_DT_SPEC_GET(USER_NODE, fem_kct8103l_ctx_gpios);

/* The pin that selects the transmit path on the fitted revision. */
static const struct gpio_dt_spec *tx_pin;

static int set_tx(bool tx)
{
	return gpio_pin_set_dt(tx_pin, tx);
}

/*
 * Small-signal gain from SX1262 output to antenna, attenuator pad
 * included, as measured by Meshtastic. Both PAs compress above about
 * 15 dBm of input, so these hold only for moderate output powers.
 */
static const struct radio_frontend gc1109 = {
	.name = "GC1109 (V4.2)",
	.tx_gain_db = 11,
	.set_tx = set_tx,
};

static const struct radio_frontend kct8103l = {
	.name = "KCT8103L (V4.3)",
	.tx_gain_db = 13,
	.set_tx = set_tx,
};

const struct radio_frontend *heltec_v4_fem_init(void)
{
	const struct radio_frontend *fe;
	int ret;

	ret = gpio_pin_configure_dt(&power, GPIO_OUTPUT_ACTIVE);
	if (ret) {
		LOG_ERR("front-end power: %d", ret);
		return NULL;
	}
	k_msleep(1);

	/* Read the revision strap before driving CSD. */
	ret = gpio_pin_configure_dt(&csd, GPIO_INPUT);
	if (ret) {
		return NULL;
	}
	k_msleep(1);

	if (gpio_pin_get_dt(&csd) == 1) {
		fe = &kct8103l;
		tx_pin = &kct8103l_ctx;
	} else {
		fe = &gc1109;
		tx_pin = &gc1109_cps;
	}

	ret = gpio_pin_configure_dt(tx_pin, GPIO_OUTPUT_INACTIVE);
	if (ret == 0) {
		ret = gpio_pin_configure_dt(&csd, GPIO_OUTPUT_ACTIVE);
	}
	if (ret) {
		LOG_ERR("front-end control pins: %d", ret);
		return NULL;
	}

	LOG_INF("RF front-end %s", fe->name);
	return fe;
}
