/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "radio.h"

LOG_MODULE_REGISTER(radio, LOG_LEVEL_INF);

/* SX1262 power amplifier range, in dBm at the chip's RF output. */
#define CHIP_POWER_MIN -9
#define CHIP_POWER_MAX 22

/*
 * Ceiling for the power at the antenna connector. 869.70-870.00 MHz allows
 * 5 mW ERP, which is 7 dBm into an antenna of up to 0 dBd (2.15 dBi). Lower
 * it by the antenna's gain above that.
 */
#define MAX_TX_POWER_DBM 7

/* Output power at the antenna until radio_set_tx_power() says otherwise. */
#define DEFAULT_TX_POWER_DBM MAX_TX_POWER_DBM

#define LINK_DATARATE SF_7

/*
 * Pairing runs at low power: both devices are meant to be next to each
 * other while the user compares LEDs. It uses a different spreading factor
 * from the link so that neither profile hears the other.
 */
#define PAIR_DATARATE     SF_8
#define PAIR_TX_POWER_DBM 0

/* Received packets waiting for radio_recv(). */
#define RX_QUEUE_DEPTH 4

static const struct device *const lora = DEVICE_DT_GET(DT_ALIAS(lora0));

/*
 * The link's modem settings. Both ends must agree on all of them.
 *
 * 869.85 MHz is the centre of the EU 869.70-870.00 MHz sub-band, the only
 * 868 MHz sub-band with no duty-cycle limit, at 5 mW ERP. At the ~200 m
 * the link needs that still leaves about 20 dB of margin at SF7, and it
 * lets the controller and every satellite transmit as often as the
 * channel allows. 250 kHz leaves 25 kHz to each band edge for crystal
 * error, and tolerates more frequency offset than 125 kHz would, which
 * suits the crystal-only RA-01SH on the satellite.
 */
static struct lora_modem_config modem = {
	.frequency = 869850000,
	.bandwidth = BW_250_KHZ,
	.datarate = LINK_DATARATE,
	.coding_rate = CR_4_5,
	.preamble_len = 8,
	.public_network = false,
	.iq_inverted = false,
};

struct rx_packet {
	struct radio_rx_info info;
	uint8_t len;
	uint8_t data[RADIO_MAX_PAYLOAD];
};

K_MSGQ_DEFINE(rx_queue, sizeof(struct rx_packet), RX_QUEUE_DEPTH, 4);

/* Serializes everything that talks to the modem. */
static K_MUTEX_DEFINE(lock);
static const struct radio_frontend *frontend;
static int8_t antenna_power_dbm;
/* What radio_set_tx_power() asked for; the link profile uses it. */
static int8_t link_power_dbm = DEFAULT_TX_POWER_DBM;
static enum radio_profile profile = RADIO_PROFILE_LINK;

/* The driver needs lora_config() whenever the direction changes. */
static enum { DIR_NONE, DIR_TX, DIR_RX } direction = DIR_NONE;

static int set_direction(bool tx)
{
	int ret;

	if (direction == (tx ? DIR_TX : DIR_RX)) {
		return 0;
	}

	modem.tx = tx;
	ret = lora_config(lora, &modem);
	if (ret) {
		LOG_ERR("lora_config(%s): %d", tx ? "tx" : "rx", ret);
		direction = DIR_NONE;
		return ret;
	}

	direction = tx ? DIR_TX : DIR_RX;
	return 0;
}

/* Runs in the driver's work queue for every packet that passes the CRC. */
static void on_rx(const struct device *dev, uint8_t *data, uint16_t size,
		  int16_t rssi, int8_t snr, void *user_data)
{
	struct rx_packet pkt = {
		.info = { .rssi_dbm = rssi, .snr_db = snr },
		.len = MIN(size, RADIO_MAX_PAYLOAD),
	};

	memcpy(pkt.data, data, pkt.len);

	if (k_msgq_put(&rx_queue, &pkt, K_NO_WAIT)) {
		LOG_WRN("rx queue full, dropped %u-byte packet", size);
	}
}

/*
 * lora_airtime() works from the driver's copy of the last TX config, which
 * it only takes on a lora_config() for TX, so hand it one whenever the modem
 * settings change, before anything asks for airtime.
 */
static int load_tx_config(void)
{
	direction = DIR_NONE;
	return set_direction(true);
}

/*
 * The receiver stays on whenever the radio is not transmitting. A blocking
 * lora_recv() with a timeout would put the modem to sleep when the timeout
 * expires and lose any packet still arriving.
 */
static int start_rx(void)
{
	int ret = set_direction(false);

	if (ret == 0) {
		ret = lora_recv_async(lora, on_rx, NULL);
	}
	return ret;
}

static void stop_rx(void)
{
	/* -EINVAL only means reception was not running. */
	(void)lora_recv_async(lora, NULL, NULL);
}

static void apply_tx_power(int8_t dbm)
{
	int8_t gain = frontend ? frontend->tx_gain_db : 0;
	int chip = CLAMP(MIN(dbm, MAX_TX_POWER_DBM) - gain, CHIP_POWER_MIN, CHIP_POWER_MAX);

	modem.tx_power = chip;
	antenna_power_dbm = chip + gain;

	/* Picked up by the next switch into TX. */
	if (direction == DIR_TX) {
		direction = DIR_NONE;
	}
}

int radio_set_tx_power(int8_t dbm)
{
	k_mutex_lock(&lock, K_FOREVER);
	link_power_dbm = dbm;
	if (profile == RADIO_PROFILE_LINK) {
		apply_tx_power(dbm);
	}
	k_mutex_unlock(&lock);
	return 0;
}

int radio_set_profile(enum radio_profile p)
{
	int ret;

	k_mutex_lock(&lock, K_FOREVER);

	if (p == profile) {
		k_mutex_unlock(&lock);
		return 0;
	}

	profile = p;
	modem.datarate = p == RADIO_PROFILE_PAIR ? PAIR_DATARATE : LINK_DATARATE;
	apply_tx_power(p == RADIO_PROFILE_PAIR ? PAIR_TX_POWER_DBM : link_power_dbm);

	/* Restart reception with the new settings. */
	stop_rx();
	ret = load_tx_config();
	if (ret == 0) {
		ret = start_rx();
	}

	LOG_INF("%s profile: SF%u, %d dBm", p == RADIO_PROFILE_PAIR ? "pairing" : "link",
		modem.datarate, antenna_power_dbm);

	k_mutex_unlock(&lock);
	return ret;
}

enum radio_profile radio_profile(void)
{
	return profile;
}

int8_t radio_tx_power(void)
{
	return antenna_power_dbm;
}

uint32_t radio_frequency(void)
{
	return modem.frequency;
}

unsigned int radio_spreading_factor(void)
{
	return modem.datarate;
}

unsigned int radio_bandwidth_khz(void)
{
	return modem.bandwidth;
}

uint32_t radio_airtime_ms(size_t len)
{
	return lora_airtime(lora, len);
}

int radio_init(const struct radio_frontend *fe)
{
	int ret;

	if (!device_is_ready(lora)) {
		LOG_ERR("%s not ready", lora->name);
		return -ENODEV;
	}

	k_mutex_lock(&lock, K_FOREVER);

	frontend = fe;
	apply_tx_power(DEFAULT_TX_POWER_DBM);

	LOG_INF("%u.%03u MHz SF%u BW%u, %d dBm at the antenna%s%s",
		modem.frequency / 1000000, (modem.frequency / 1000) % 1000,
		modem.datarate, modem.bandwidth, antenna_power_dbm,
		fe ? " via " : "", fe ? fe->name : "");

	ret = load_tx_config();
	if (ret == 0) {
		ret = start_rx();
	}

	k_mutex_unlock(&lock);
	return ret;
}

int radio_send(const void *buf, size_t len)
{
	int ret;

	if (len > RADIO_MAX_PAYLOAD) {
		return -EMSGSIZE;
	}

	k_mutex_lock(&lock, K_FOREVER);

	stop_rx();

	ret = set_direction(true);
	if (ret == 0 && frontend) {
		ret = frontend->set_tx(true);
	}
	if (ret == 0) {
		/* The API takes a non-const buffer but does not write to it. */
		ret = lora_send(lora, (uint8_t *)buf, len);
	}
	if (frontend) {
		frontend->set_tx(false);
	}

	int rx_ret = start_rx();

	k_mutex_unlock(&lock);
	return ret ? ret : rx_ret;
}

int radio_recv(void *buf, size_t size, k_timeout_t timeout,
	       struct radio_rx_info *info)
{
	struct rx_packet pkt;

	if (k_msgq_get(&rx_queue, &pkt, timeout)) {
		return -EAGAIN;
	}

	if (pkt.len > size) {
		return -EMSGSIZE;
	}

	memcpy(buf, pkt.data, pkt.len);
	*info = pkt.info;
	return pkt.len;
}
