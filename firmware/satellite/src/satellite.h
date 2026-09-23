/* SPDX-License-Identifier: Apache-2.0 */

#ifndef SATELLITE_H_
#define SATELLITE_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define SAT_CHANNELS 6

/* Session key material, loaded from the storage partition at boot and
 * replaced over USB at the start of an event.
 */
int keys_load(void);
int keys_provision(const uint8_t *key, size_t len);
int keys_seal(const uint8_t *plain, size_t len, uint8_t *out, size_t *out_len);
int keys_unseal(const uint8_t *sealed, size_t len, uint8_t *out, size_t *out_len);

/* LoRa link to the operator console. */
int link_init(void);
int link_send(const uint8_t *buf, size_t len);
int link_recv(uint8_t *buf, size_t len, int32_t timeout_ms);

/* Pyro channels: high-side enable, low-side fire, continuity sense. */
int channels_init(void);
int channels_arm(int ch, bool on);
int channels_fire(int ch, uint32_t ms);
int channels_continuity(int ch, int32_t *mv);
int channels_rail_mv(int input, int32_t *mv);
int channels_diagnostics(bool on);
int channels_latch_clear(void);

/* INA226 on the fire rail shunt. */
int rail_init(void);
int rail_read(int32_t *mv, int32_t *ma);

#endif /* SATELLITE_H_ */
