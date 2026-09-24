/*
 * Command and status payloads as they travel inside sealed lsec frames:
 * the commands inside the controller's poll, and each satellite's status
 * in its reply slot.
 *
 * Provisional: enough for the controller and the simulator to exercise the
 * secure link end to end. The status is too big for more than two
 * satellites at the controller's 250 ms cadence; see design/radio_security.md,
 * "Channel access".
 */

#ifndef LINK_WIRE_H_
#define LINK_WIRE_H_

#include <stddef.h>
#include <stdint.h>

#include "link.h"

/* A poll: a type byte, then one 3-byte command per addressed satellite. */
#define WIRE_POLL_CMD_LEN 3
#define WIRE_POLL_MAX_LEN (1 + NUM_SATS * WIRE_POLL_CMD_LEN)
#define WIRE_STATUS_LEN   45

/*
 * The commands for the satellites in @p mask (bit n = satellite n), in
 * the order of the set bits, which is the order of their reply slots.
 */
size_t wire_encode_poll(const struct link_cmd cmds[NUM_SATS], uint8_t mask, uint8_t *out);
/* Take the command for the satellite at @p rank of @p count addressed. */
int wire_decode_poll(const uint8_t *buf, size_t len, uint8_t rank, uint8_t count,
		     struct link_cmd *cmd);

size_t wire_encode_status(const struct sat_status *st, uint8_t *out);
int wire_decode_status(const uint8_t *buf, size_t len, struct sat_status *st);

#endif /* LINK_WIRE_H_ */
