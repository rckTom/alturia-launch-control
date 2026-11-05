/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef OLED_H_
#define OLED_H_

#include <stdbool.h>
#include <stdint.h>

/* What the bench shows on the board's 128x64 OLED. Owned by main(). */
struct bench_status {
	uint16_t node_id;
	const char *frontend;   /* NULL until the front-end is detected */

	/* Last packet heard, of any kind. rx_time is 0 until the first. */
	int64_t rx_time;
	uint8_t rx_type;        /* enum bench_pkt_type, 0 if unrecognized */
	uint32_t rx_seq;
	uint16_t rx_src;
	int16_t rx_rssi_dbm;
	int8_t rx_snr_db;

	/* Current or most recent ping run. ping_total is 0 before the first. */
	bool pinging;
	uint32_t ping_sent;
	uint32_t ping_total;
	uint32_t pongs;

	/* Most recent answered ping. */
	int16_t there_rssi_dbm;
	int8_t there_snr_db;
	int16_t back_rssi_dbm;
	int8_t back_snr_db;
	uint32_t rtt_ms;

	/* Set when bring-up fails; replaces the radio lines. */
	const char *error;
	int error_code;
};

struct secure_status;

/* Returns -ENODEV if there is no display; the bench runs without one. */
int oled_init(void);
void oled_show(const struct bench_status *st);
void oled_show_secure(uint16_t node_id, const struct secure_status *st);

#endif /* OLED_H_ */
