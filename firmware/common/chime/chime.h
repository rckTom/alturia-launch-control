/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef CHIME_H_
#define CHIME_H_

/*
 * Tunes on the board's PWM buzzer (devicetree alias "buzzer"), shared by
 * the controller and the satellite. None of them blocks; a new tune
 * replaces the one playing. All are no-ops on a board without a buzzer.
 */

/* Start-up tune on the buzzer. */
void chime_startup(void);
/* Falling tune: start-up failed, see the log; or a fault. */
void chime_error(void);

/* Controller: arming worked, and the pads are disarmed again */
void chime_armed(void);
void chime_disarm(void);
/* Controller: a click, repeated while armed */
void chime_armed_tick(void);
/* Controller: the satellites confirmed the launch */
void chime_launch(void);
/* Controller: a button was refused */
void chime_refused(void);

#endif /* CHIME_H_ */
