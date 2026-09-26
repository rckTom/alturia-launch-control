/* SPDX-License-Identifier: Apache-2.0 */

#ifndef SATELLITE_H_
#define SATELLITE_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct lsec_event;

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
uint8_t channels_armed_mask(void);
/*
 * Drive the low sides of the armed channels in @p mask for @p ms, without
 * blocking. -EPERM if one of them is not armed, -EBUSY during a pulse.
 */
int channels_pulse(uint8_t mask, uint32_t ms);
uint8_t channels_pulsing(void);
/* Hold the low sides of the armed channels in @p mask on (valves), the rest off. */
int channels_valves(uint8_t mask);
uint8_t channels_valve_mask(void);
int channels_continuity(int ch, int32_t *mv);
int channels_rail_mv(int input, int32_t *mv);
int channels_diagnostics(bool on);
int channels_latch_clear(void);

/*
 * Commands from the console (common/wire/sat_wire.h). Called from the
 * LSEC_EV_POLL handler on the link thread: executes the command for this
 * satellite, once per request id, and queues the status as the reply.
 */
void commands_on_poll(const struct lsec_event *ev);
/* Refresh the measurements the status reports; from the main loop. */
void commands_measure(void);
/* Disarm everything, as a command would. */
void commands_disarm(const char *why);

/* INA226 on the fire rail shunt. */
int rail_init(void);
int rail_read(int32_t *mv, int32_t *ma);

#endif /* SATELLITE_H_ */
