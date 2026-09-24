/* SPDX-License-Identifier: Apache-2.0 */

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

#endif /* TRACE_H_ */
