/*
 * Physical controls of the controller (keyboard keys on native_sim).
 * Momentary buttons (ARM, SAFE, FILL, VENT, encoder, keypad) are posted to the
 * controller directly; the level inputs below are polled.
 */

#ifndef INPUTS_H_
#define INPUTS_H_

#include <stdbool.h>

bool inputs_key_on(void);
bool inputs_fire_held(void);

/* Overrides for the shell, e.g. when the SDL window has no focus. */
void inputs_set_key(bool on);
void inputs_set_fire(bool held);

#endif /* INPUTS_H_ */
