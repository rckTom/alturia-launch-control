/* SPDX-License-Identifier: Apache-2.0 */

/*
 * LoRa radio backend shared by the console and the satellite.
 *
 * A thin layer over Zephyr's LoRa API that owns the one radio on the
 * devicetree alias lora0. It keeps the modem settings in one place, keeps
 * the receiver on whenever it is not transmitting, hides the TX/RX
 * reconfiguration the driver needs between directions, and drives an
 * optional RF front-end (PA/LNA) around each transmission.
 *
 * Any thread may call it; access to the modem is serialized internally.
 */

#ifndef LORA_RADIO_H_
#define LORA_RADIO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>

/* Largest payload the SX126x accepts in one packet. */
#define RADIO_MAX_PAYLOAD 255

/*
 * An external front-end between the transceiver and the antenna, such as
 * the PA/LNA on the Heltec V4. Boards without one pass NULL to radio_init().
 */
struct radio_frontend {
	const char *name;
	/* Net gain from transceiver output to antenna while transmitting. */
	int8_t tx_gain_db;
	/* Called with true before a transmission and false after it. */
	int (*set_tx)(bool tx);
};

struct radio_rx_info {
	int16_t rssi_dbm;
	int8_t snr_db;
	int64_t time_us;   /* uptime when the packet had been received */
};

int radio_init(const struct radio_frontend *fe);

/* Transmit one packet and block until it has left the antenna. */
int radio_send(const void *buf, size_t len);

/*
 * Take the next received packet, waiting up to @p timeout for one.
 * Packets arriving between calls are queued, a few deep. Returns the
 * packet's length, -EAGAIN if none arrived, or -EMSGSIZE if it was longer
 * than @p size and has been dropped. Packets that fail the PHY CRC never
 * get this far.
 */
int radio_recv(void *buf, size_t size, k_timeout_t timeout,
	       struct radio_rx_info *info);

/*
 * Make a thread waiting in radio_recv() return 0 at once, so that it can
 * look at other work without polling.
 */
void radio_wake(void);

/*
 * Output power at the antenna connector, front-end gain included. It is
 * capped at 7 dBm for the 5 mW ERP band and the transceiver is clamped to
 * its -9..22 dBm range, so the result can differ from the request;
 * radio_tx_power() reports what was applied.
 */
int radio_set_tx_power(int8_t dbm);
int8_t radio_tx_power(void);

/*
 * Modem settings. LINK is the normal link (SF7, radio_set_tx_power()).
 * PAIR is SF8 at low power, for pairing at arm's length. Both ends must
 * be on the same profile to hear each other.
 */
enum radio_profile {
	RADIO_PROFILE_LINK,
	RADIO_PROFILE_PAIR,
};

int radio_set_profile(enum radio_profile p);
enum radio_profile radio_profile(void);

/*
 * Listen for @p listen_us and say whether the channel is free: 1 clear,
 * 0 busy, negative if it could not be sampled. Busy means an RSSI sample
 * above the noise floor plus @p margin_db. The floor is measured at init
 * and on each profile change, and follows the quietest samples after that.
 * @p peak_dbm (may be NULL) gets the loudest sample.
 */
int radio_channel_clear(uint32_t listen_us, int margin_db, int16_t *peak_dbm);
int16_t radio_noise_floor(void);

uint32_t radio_frequency(void);
unsigned int radio_spreading_factor(void);
unsigned int radio_bandwidth_khz(void);
uint32_t radio_airtime_ms(size_t len);

#endif /* LORA_RADIO_H_ */
