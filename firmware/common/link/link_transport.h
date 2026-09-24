/* SPDX-License-Identifier: Apache-2.0 */

/*
 * What ctrl_link and sat_link send and receive frames through: the LoRa
 * radio on real hardware (link_radio_transport), or the controller
 * simulator's in-process air on native_sim.
 */

#ifndef LINK_TRANSPORT_H_
#define LINK_TRANSPORT_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>

enum link_profile {
	LINK_PROFILE_LINK,   /* normal range and power */
	LINK_PROFILE_PAIR,   /* short range, for pairing */
};

struct link_rx_info {
	int16_t rssi_dbm;
	int8_t snr_db;
	int64_t time_us;   /* uptime when reception ended; reply slots count from it */
};

struct link_transport {
	/* Transmit one frame and block until it is gone. */
	int (*send)(const void *buf, size_t len);
	/* Wait up to @p timeout for a frame. Returns its length or -EAGAIN. */
	int (*recv)(void *buf, size_t size, k_timeout_t timeout, struct link_rx_info *info);
	/* Both ends hear each other only on the same profile. */
	int (*set_profile)(enum link_profile p);
	enum link_profile (*profile)(void);
	/*
	 * Listen for @p listen_us: 1 if the channel is clear, 0 if busy,
	 * negative if it cannot tell (treat as clear). @p peak_dbm, which may
	 * be NULL, gets the loudest level heard.
	 */
	int (*channel_clear)(uint32_t listen_us, int16_t *peak_dbm);
	/* Time on air of a @p len byte frame on the current profile. */
	uint32_t (*airtime_ms)(size_t len);
};

/* The SX126x through ../lora/radio.c. radio_init() must have run. */
extern const struct link_transport link_radio_transport;

#endif /* LINK_TRANSPORT_H_ */
