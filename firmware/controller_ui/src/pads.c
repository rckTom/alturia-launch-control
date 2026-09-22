#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "pads.h"

LOG_MODULE_REGISTER(pads, LOG_LEVEL_INF);

/* Defaults for a new controller, replaced by whatever is stored */
struct pad_cfg pads[NUM_PADS] = {
	/* Satellite A: one hybrid pad and three rocket pads */
	{.type = PAD_HYBRID, .sat = 0, .ign = 0, .fill = 4, .vent = 5},
	{.type = PAD_ROCKET, .sat = 0, .ign = 1},
	{.type = PAD_ROCKET, .sat = 0, .ign = 2},
	{.type = PAD_ROCKET, .sat = 0, .ign = 3},
	/* Satellite B: one hybrid pad and two rocket pads, CH4 unused */
	{.type = PAD_HYBRID, .sat = 1, .ign = 0, .fill = 4, .vent = 5},
	{.type = PAD_ROCKET, .sat = 1, .ign = 1},
	{.type = PAD_ROCKET, .sat = 1, .ign = 2},
};

static bool cfg_valid(const struct pad_cfg *cfg)
{
	return cfg->type < PAD_TYPE_COUNT && cfg->sat < NUM_SATS &&
	       cfg->ign < NUM_CHANNELS && cfg->fill < NUM_CHANNELS &&
	       cfg->vent < NUM_CHANNELS && cfg->label[PAD_LABEL_MAX] == '\0';
}

static int pads_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	int p = atoi(name) - 1;
	struct pad_cfg cfg;

	if (p < 0 || p >= NUM_PADS || len != sizeof(cfg)) {
		LOG_WRN("ignoring stored pad/%s", name);
		return 0;
	}
	if (read_cb(cb_arg, &cfg, sizeof(cfg)) != sizeof(cfg) || !cfg_valid(&cfg)) {
		LOG_WRN("pad %d: stored configuration is invalid, using default", p + 1);
		return 0;
	}
	pads[p] = cfg;
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(pad, "pad", NULL, pads_set, NULL, NULL);

int pads_init(void)
{
	int err = settings_subsys_init();

	if (err) {
		LOG_ERR("settings init failed (%d), pad setup will not persist", err);
		return err;
	}
	err = settings_load_subtree("pad");

	/* Stored pads are checked one by one; make sure they fit together */
	for (int p = 0; p < NUM_PADS; p++) {
		const char *why = pads_check(p, &pads[p]);

		if (why) {
			LOG_ERR("pad %d disabled: %s", p + 1, why);
			pads[p].type = PAD_OFF;
		}
	}
	return err;
}

const char *pads_check(int p, const struct pad_cfg *cfg)
{
	static char buf[48];
	uint8_t used = pad_channels(cfg);

	if (cfg->type == PAD_HYBRID &&
	    (cfg->ign == cfg->fill || cfg->ign == cfg->vent || cfg->fill == cfg->vent)) {
		return "Igniter, fill and vent need different channels";
	}

	for (int other = 0; other < NUM_PADS; other++) {
		uint8_t clash;

		if (other == p || pads[other].sat != cfg->sat) {
			continue;
		}
		clash = used & pad_channels(&pads[other]);
		if (clash) {
			snprintf(buf, sizeof(buf), "CH%d on satellite %c is used by pad %d",
				 __builtin_ctz(clash) + 1, sat_name(cfg->sat),
				 other + 1);
			return buf;
		}
	}
	return NULL;
}

int pads_store(int p, const struct pad_cfg *cfg)
{
	char key[24];

	pads[p] = *cfg;
	snprintf(key, sizeof(key), "pad/%d", p + 1);
	return settings_save_one(key, &pads[p], sizeof(pads[p]));
}

const char *pad_type_name(enum pad_type type)
{
	switch (type) {
	case PAD_ROCKET:
		return "ROCKET";
	case PAD_HYBRID:
		return "HYBRID";
	default:
		return "OFF";
	}
}

const char *pad_name(int p)
{
	static char numbered[NUM_PADS][16];

	if (pads[p].label[0]) {
		return pads[p].label;
	}
	snprintf(numbered[p], sizeof(numbered[p]), "Pad %d", p + 1);
	return numbered[p];
}

void pads_format(char *buf, size_t len, uint16_t mask)
{
	size_t n = 0;

	buf[0] = '\0';
	for (int p = 0; p < NUM_PADS && n < len; p++) {
		if (mask & BIT(p)) {
			n += snprintf(buf + n, len - n, "%s%s", n ? ", " : "", pad_name(p));
		}
	}
}
