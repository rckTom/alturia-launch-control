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

struct link_transport {
	/* Transmit one frame and block until it is gone. */
	int (*send)(const void *buf, size_t len);
	/* Wait up to @p timeout for a frame. Returns its length or -EAGAIN. */
	int (*recv)(void *buf, size_t size, k_timeout_t timeout, int16_t *rssi_dbm,
		    int8_t *snr_db);
	/* Both ends hear each other only on the same profile. */
	int (*set_profile)(enum link_profile p);
	enum link_profile (*profile)(void);
};

/* The SX126x through ../lora/radio.c. radio_init() must have run. */
extern const struct link_transport link_radio_transport;

#endif /* LINK_TRANSPORT_H_ */
