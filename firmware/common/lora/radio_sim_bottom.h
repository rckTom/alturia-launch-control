/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Host side of radio_sim.c: the UDP socket to the network simulator and
 * the host clock. Built into the native simulator runner against the host
 * C library, so no Zephyr headers here and no Zephyr errno values: calls
 * return -1 on failure.
 */

#ifndef RADIO_SIM_BOTTOM_H_
#define RADIO_SIM_BOTTOM_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open a non-blocking UDP socket connected to host:port. 0 or -1. */
int rsim_bottom_open(const char *host, int port);

/* Send one datagram. The length sent, or -1. */
int rsim_bottom_send(const void *buf, int len);

/* Take one datagram if there is one: its length, 0 if none, -1 on error. */
int rsim_bottom_recv(void *buf, int size);

/* Host CLOCK_MONOTONIC in microseconds. */
uint64_t rsim_bottom_now_us(void);

long rsim_bottom_getpid(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_SIM_BOTTOM_H_ */
