/* Fault-injection hooks into the simulated satellites (shell only). */

#ifndef SAT_SIM_H_
#define SAT_SIM_H_

#include <stdbool.h>
#include <stdint.h>

/* sat is 0-based, ch is 0-based */
void sat_sim_set_link(int sat, bool up);
void sat_sim_set_loss(int sat, int percent);
void sat_sim_set_key(int sat, bool armed);
void sat_sim_set_estop(int sat, bool pressed);
void sat_sim_set_res(int sat, int ch, int32_t mohm);
void sat_sim_set_vbat(int sat, int32_t mv);
void sat_sim_print(int sat, void (*print)(const char *fmt, ...));

#endif /* SAT_SIM_H_ */
