/*
 * Physical controls of the controller (keyboard keys on native_sim).
 * Momentary buttons (ARM, FILL, VENT, the D-pad, BACK, power) are posted
 * to the controller directly; the level inputs below are polled.
 */

#ifndef INPUTS_H_
#define INPUTS_H_

#include <stdbool.h>
#include <stdint.h>

bool inputs_fire_held(void);

/* Override for the shell, e.g. when the SDL window has no focus. */
void inputs_set_fire(bool held);

/* Every button, for the button test screen */
enum button {
	BTN_ARM,
	BTN_FIRE,
	BTN_FILL,
	BTN_VENT,
	BTN_UP,
	BTN_DOWN,
	BTN_LEFT,
	BTN_RIGHT,
	BTN_OK,
	BTN_BACK,
	BTN_POWER,
	BTN_COUNT,
};

const char *button_name(enum button b);

#define INPUTS_LOG_LEN 4

struct button_event {
	uint8_t button;
	bool pressed;
	int64_t time;      /* uptime, ms */
	int32_t held_ms;   /* on release: how long it was down */
};

struct inputs_diag {
	uint32_t pressed;               /* bit per enum button */
	uint32_t count[BTN_COUNT];      /* presses since boot */
	int n_log;                      /* entries in log, newest first */
	struct button_event log[INPUTS_LOG_LEN];
};

void inputs_diag(struct inputs_diag *out);

bool inputs_pressed(enum button b);

#endif /* INPUTS_H_ */
