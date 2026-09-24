/* SPDX-License-Identifier: Apache-2.0 */

#ifndef SATELLITE_H_
#define SATELLITE_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define SAT_CHANNELS 6

/*
 * The secure LoRa link to the console is ../common/link/sat_link.h. This
 * is its local half: the pairing gesture on the button, the pattern on the
 * channel LEDs, and the confirmation. See design/radio_security.md.
 */
int pair_ui_init(void);
/* Start pairing mode, if the satellite is SAFE. */
int pair_ui_start(void);
/* Show the pattern to compare, or stop showing it (sas == NULL). */
void pair_ui_show(const uint8_t *sas);

/* Pyro channels: high-side enable, low-side fire, continuity sense. */
int channels_init(void);
int channels_arm(int ch, bool on);
/* No channel armed: the state in which pairing is allowed. */
bool channels_all_safe(void);
int channels_fire(int ch, uint32_t ms);
int channels_continuity(int ch, int32_t *mv);
int channels_rail_mv(int input, int32_t *mv);
int channels_diagnostics(bool on);
int channels_latch_clear(void);

/* INA226 on the fire rail shunt. */
int rail_init(void);
int rail_read(int32_t *mv, int32_t *ma);

#endif /* SATELLITE_H_ */
