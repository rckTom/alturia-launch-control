/*
 * Switching the controller off through its soft power latch
 * (hardware/controller power_latch sheet).
 */

#ifndef POWER_H_
#define POWER_H_

#include <stdbool.h>

/* The board can switch itself off */
bool power_can_off(void);

/*
 * Drop the latch, after a moment for the shutdown splash. Waits for the
 * power button to be released first: while it is held, it keeps the latch
 * set. Returns at once.
 */
void power_off(void);

#endif /* POWER_H_ */
