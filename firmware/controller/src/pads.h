/*
 * Launch pad configuration: which satellite outputs belong to which pad,
 * and the operator's label for it. Edited on the controller's pad setup
 * screen and persisted with the settings subsystem under "pad/<n>".
 *
 * A rocket pad has one igniter. A hybrid pad has an igniter, a fill valve
 * (normally closed, energize to open) and a vent valve (normally open,
 * energize to close), so dropping arm on the satellite always vents.
 * An OFF pad uses no channels and cannot be selected.
 */

#ifndef PADS_H_
#define PADS_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include "link.h"

/* Enough for every channel of every satellite to be a rocket pad */
#define NUM_PADS      (NUM_SATS * NUM_CHANNELS)
#define PAD_LABEL_MAX 4

enum pad_type {
	PAD_OFF,
	PAD_ROCKET,
	PAD_HYBRID,
	PAD_TYPE_COUNT,
};

struct pad_cfg {
	uint8_t type;   /* enum pad_type */
	uint8_t sat;
	/* 0-based satellite channels; fill and vent only for PAD_HYBRID */
	uint8_t ign;
	uint8_t fill;
	uint8_t vent;
	char label[PAD_LABEL_MAX + 1];
};

/* Current configuration. Only change it through pads_store(). */
extern struct pad_cfg pads[NUM_PADS];

/* Load the stored configuration. Call once before the UI starts. */
int pads_init(void);

/*
 * Check a candidate configuration for pad p against itself and all other
 * enabled pads. Returns NULL if it is valid, otherwise why not.
 */
const char *pads_check(int p, const struct pad_cfg *cfg);

/* Apply and persist a configuration that passed pads_check(). */
int pads_store(int p, const struct pad_cfg *cfg);

const char *pad_type_name(enum pad_type type);

/* All satellite channels used by a pad */
static inline uint8_t pad_channels(const struct pad_cfg *p)
{
	switch (p->type) {
	case PAD_ROCKET:
		return BIT(p->ign);
	case PAD_HYBRID:
		return BIT(p->ign) | BIT(p->fill) | BIT(p->vent);
	default:
		return 0;
	}
}

/* What operators see: the label if one is set, otherwise "Pad <n>" */
const char *pad_name(int p);

/*
 * "ALFA, Pad 2, BETA" for a bitmask of pads (bit p = pad p). What does not
 * fit in @p len is counted at the end instead: "ALFA, Pad 2 +5".
 */
void pads_format(char *buf, size_t len, uint64_t mask);

/* A sensible size for pads_format(), a line on the display */
#define PADS_FORMAT_LEN 48

static inline char sat_name(int sat)
{
	return 'A' + sat;
}

#endif /* PADS_H_ */
