/* SPDX-License-Identifier: Apache-2.0 */

#ifndef BENCH_H_
#define BENCH_H_

#include <stdint.h>

/*
 * Test packet. Every board answers a PING with a PONG that reports how
 * well it heard the PING, so the pinging board sees both directions of
 * the link. Fields are little-endian, as are both MCUs in the project.
 */
#define BENCH_MAGIC 0xB5

enum bench_pkt_type {
	BENCH_PING = 1,
	BENCH_PONG = 2,
	BENCH_POLL = 3,   /* secure mode: the payload of a link poll */
};

struct bench_pkt {
	uint8_t magic;
	uint8_t type;
	uint16_t src;      /* sender's node id */
	/* PONG: the pinging node. PING: 0, anyone answers. POLL: pad the PONG to this length. */
	uint16_t dst;
	uint32_t seq;
	int16_t rssi_dbm;  /* PONG: how the PING was heard */
	int8_t snr_db;
} __packed;

/* Node id: the low 16 bits of the chip's factory MAC/serial. */
uint16_t bench_node_id(void);

/*
 * Ask the radio loop to send @p count pings, one per @p interval_ms.
 * Returns -EBUSY if a ping run is already queued or in progress.
 */
int bench_ping_start(uint32_t count, uint32_t interval_ms);
void bench_ping_stop(void);

#endif /* BENCH_H_ */
