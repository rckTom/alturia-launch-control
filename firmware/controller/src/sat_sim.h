/* The simulated satellites: their air, and fault-injection hooks for the shell. */

#ifndef SAT_SIM_H_
#define SAT_SIM_H_

#include <stdbool.h>
#include <stdint.h>

#include "link_transport.h"

/*
 * Start the simulated satellites and return the controller's end of the
 * simulated air, to hand to ctrl_link_init(). Needs settings loaded.
 */
const struct link_transport *sat_sim_transport(void);

/* The satellite's pairing gesture, and its user's verdict on the LEDs. */
void sat_sim_pair(int sat);
void sat_sim_pair_confirm(int sat, bool accept);
void sat_sim_unpair(int sat);

/* sat is 0-based, ch is 0-based */
void sat_sim_set_link(int sat, bool up);
void sat_sim_set_loss(int sat, int percent);
void sat_sim_set_key(int sat, bool armed);
void sat_sim_set_res(int sat, int ch, int32_t mohm);
void sat_sim_set_vbat(int sat, int32_t mv);
void sat_sim_print(int sat, void (*print)(const char *fmt, ...));

#endif /* SAT_SIM_H_ */
