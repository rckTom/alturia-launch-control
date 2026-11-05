/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * native_sim only: extras of the simulated radio (radio_sim.c) beyond
 * radio.h, for application glue that mirrors a node's LEDs, display and
 * controls in the network simulator's web UI (tools/radiosim).
 */

#ifndef LORA_RADIO_SIM_H_
#define LORA_RADIO_SIM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "radio_sim_proto.h"

/* The node's LEDs as a bitmask; sent to the simulator when it changes. */
void radio_sim_report_leds(uint16_t mask);

/*
 * Send one whole display frame, @p stride bytes per row, in one of the
 * rsim_pixfmt formats. -EMSGSIZE if it does not fit in one datagram.
 */
int radio_sim_send_display(uint16_t width, uint16_t height, enum rsim_pixfmt format,
			   uint16_t stride, const void *buf, size_t len);

/*
 * Inputs from the web UI. Called on the system work queue: @p index and
 * @p pressed for a node's own button, rsim_input_type events otherwise.
 */
void radio_sim_set_button_cb(void (*cb)(uint8_t index, bool pressed));
void radio_sim_set_input_cb(void (*cb)(enum rsim_input_type type, uint16_t code, int32_t value));

/* Whether the simulator has answered within the last few seconds. */
bool radio_sim_connected(void);

#endif /* LORA_RADIO_SIM_H_ */
