/* SPDX-License-Identifier: Apache-2.0 */

#ifndef SATELLITE_H_
#define SATELLITE_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "chime.h"

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

struct cont_result {
	int32_t mohm;		/* INT32_MAX when open */
	int32_t pyro_h_uv;	/* measured at the output */
	int32_t v_test_uv;	/* under load */
	int32_t v_test_model_uv;	/* under load, from R12's sag */
	int v_test_from;	/* channel V_TEST was measured on, -1: modelled */
	int32_t diode_uv;	/* series diode drop under load */
	int32_t zero_uv;	/* ADC reading with the output at 0 V, subtracted */
	int32_t current_na;	/* through the load */
	bool open;
};
/*
 * Resistance across channel @p ch's output: switches V_TEST and the
 * channel's low side on, measures, and switches both off again. -EBUSY
 * while the channel is armed; -ENOTSUP without the hardware; -EIO without
 * V_TEST; -ERANGE, with @p res filled in, when the readings do not fit the
 * circuit.
 */
int channels_continuity_mohm(int ch, struct cont_result *res);
/*
 * The high-side switch of channel @p ch, for ARM with K1 closed: the tap
 * (PYRO_H / 4.3) with the high side off, then on, the low side off all
 * along so that nothing flows through the igniter. -EBUSY while armed,
 * -ENOTSUP without the hardware.
 */
int channels_check_high(int ch, int32_t *off_uv, int32_t *on_uv);
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
/*
 * The latest continuity per output, as bit masks: reading OK, and shorted
 * or not measurable. Lock-free, for the LEDs' work item.
 */
void commands_cont_masks(uint8_t *ok, uint8_t *bad);

/* Arm relay K1: ARM_ENABLE and the ARM_PUMP one-shot (needs the key on). */
int arm_init(void);
int arm_relay_set(bool on);
bool arm_relay_on(void);
/* KEY_ENABLED: 1 when the key relay K2 has closed, or -errno. */
int arm_key_on(void);

/* Battery voltage, VBAT_FUSED. There is no fire-rail current monitor:
 * the TPS1HB08 current sense only sees current while a channel conducts.
 */
int rail_vbat_mv(int32_t *mv);

/* The measurable supply rails, by index: name and voltage. */
size_t rails_count(void);
int rails_read(size_t i, const char **name, int32_t *mv);

/* fault.c: the fatal error that rebooted the satellite, if any. */
struct shell;
void fault_print(const struct shell *sh);

#endif /* SATELLITE_H_ */
