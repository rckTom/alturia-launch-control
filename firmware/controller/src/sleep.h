/*
 * Light sleep between ticks and radio events (CONFIG_PM), see sleep.c.
 */

#ifndef SLEEP_H_
#define SLEEP_H_

#include <stdbool.h>
#include <stdint.h>

struct sleep_stats {
	bool allowed;           /* not switched off from the shell */
	bool usb;               /* USB power present: no sleep */
	uint32_t entries;       /* times the kernel went for standby */
	uint32_t timer_wakes;
	uint32_t gpio_wakes;    /* a key, the radio or USB */
	uint64_t slept_ms;      /* in standby */
	uint64_t unlocked_ms;   /* could have slept: on battery, allowed */
};

#ifdef CONFIG_PM
/* Allow or forbid light sleep, for measuring the current both ways */
void sleep_allow(bool allow);
void sleep_get_stats(struct sleep_stats *out);
void sleep_reset_stats(void);
#endif

#endif /* SLEEP_H_ */
