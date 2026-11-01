/*
 * The controller's own battery: MAX17048 fuel gauge, BQ24296M charger and
 * the charger's power-good line, read every couple of seconds.
 */

#ifndef BATTERY_H_
#define BATTERY_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/charger.h>

struct battery_info {
	/* Fuel gauge */
	bool gauge;               /* the readings below are valid */
	uint8_t soc_pct;
	uint32_t voltage_mv;
	uint32_t to_empty_min;    /* 0: not discharging */
	uint32_t to_full_min;     /* 0: not charging */

	/* Charger */
	bool charger;             /* the readings below are valid */
	bool online;              /* input power present */
	enum charger_status status;
	enum charger_health health;
	uint32_t ichg_ma;         /* fast charge current */
	uint32_t vreg_mv;         /* charge voltage */
	uint32_t iinlim_ma;       /* input current limit */
	uint32_t vindpm_mv;       /* input voltage limit */
	uint32_t iprechg_ma;
	uint32_t iterm_ma;

	int pgood;                /* USB_PGOOD: 1 good, 0 not, -1 no such line */
};

void battery_get(struct battery_info *out);

/* Charging, as the charger reports it */
bool battery_charging(const struct battery_info *b);

const char *battery_status_text(enum charger_status status);
const char *battery_health_text(enum charger_health health);

/*
 * Stand-in readings for boards without a fuel gauge (native_sim), to try
 * the UI from the shell.
 */
int battery_fake(uint8_t soc_pct, bool charging);

#endif /* BATTERY_H_ */
