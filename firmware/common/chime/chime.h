/* SPDX-License-Identifier: Apache-2.0 */

#ifndef CHIME_H_
#define CHIME_H_

/*
 * Tunes on the board's PWM buzzer (devicetree alias "buzzer"), shared by
 * the controller and the satellite. Both calls are no-ops on a board
 * without one.
 */

/* Start-up tune on the buzzer, without blocking. */
void chime_startup(void);
/* Falling tune: start-up failed, see the log. */
void chime_error(void);

#endif /* CHIME_H_ */
