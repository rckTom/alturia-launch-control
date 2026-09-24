/*
 * Command and status payloads as they travel inside sealed lsec frames.
 *
 * Provisional: enough for the controller and the simulator to exercise the
 * secure link end to end. The real encoding will be settled together with
 * the airtime budget (design/radio_security.md, "Out of scope").
 */

#ifndef LINK_WIRE_H_
#define LINK_WIRE_H_

#include <stddef.h>
#include <stdint.h>

#include "link.h"

#define WIRE_CMD_LEN    4
#define WIRE_STATUS_LEN 45

size_t wire_encode_cmd(const struct link_cmd *cmd, uint8_t *out);
int wire_decode_cmd(const uint8_t *buf, size_t len, struct link_cmd *cmd);

size_t wire_encode_status(const struct sat_status *st, uint8_t *out);
int wire_decode_status(const uint8_t *buf, size_t len, struct sat_status *st);

#endif /* LINK_WIRE_H_ */
