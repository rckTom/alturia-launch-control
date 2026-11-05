/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * Frame trace for the secure link: a link_transport that passes every
 * frame through to link_radio_transport and logs it on the way, both
 * directions, decoded as far as the clear header goes. Sealed bodies stay
 * opaque. Frames that fail the PHY CRC never reach it.
 */

#ifndef TRACE_H_
#define TRACE_H_

#include "link_transport.h"

enum trace_mode {
	TRACE_OFF,
	TRACE_ON,    /* one line per frame */
	TRACE_HEX,   /* and a hex dump of it */
};

extern const struct link_transport trace_transport;

void trace_set_mode(enum trace_mode mode);
enum trace_mode trace_mode(void);

/*
 * Test hook: right after each poll, transmit a raw frame about 200 ms
 * long. It starts after the first slot's listen, so put the satellite
 * behind phantoms ("bench poll 20 400 0x07") to see it find the slot busy.
 */
void trace_set_jam_slot(bool on);

#endif /* TRACE_H_ */
