/*
 * Controller state machine. Owns every arm/fire/valve decision; the UI
 * only reads ctrl_view(). User intent arrives through ctrl_post(), from
 * the button input callback or the shell.
 * ctrl_tick() and ctrl_view() run in the LVGL thread.
 *
 * Operators select pads, not channels. Arming arms every satellite that
 * hosts a selected pad. FIRE launches all selected pads at once. On
 * hybrid pads, FILL and VENT toggle the valves while armed; either one,
 * and LAUNCH, stops a running fill.
 *
 * The controls are a D-pad (up, down, left, right, OK), BACK, ARM, FIRE,
 * FILL, VENT and the power button. BACK disarms, acknowledges a fault and
 * backs out of a screen.
 *
 * In SAFE, holding OK opens the settings menu:
 *  - Pads: all pads including OFF ones (the main list only shows enabled
 *    pads); OK on one opens its setup screen.
 *  - Satellites: pairing and forgetting.
 *  - Diagnostics, a menu of its own: the button test, which shows every
 *    button press instead of acting on it (holding BACK leaves), and the
 *    battery.
 *  - Power off.
 * While any of these is open, the D-pad acts on it, BACK backs out one
 * level, and ARM, FILL and VENT are refused. The power button asks to
 * switch off from anywhere in SAFE.
 */

#ifndef CONTROLLER_H_
#define CONTROLLER_H_

#include <stdbool.h>
#include <stdint.h>

#include "link.h"
#include "pads.h"

enum ctrl_state {
	CTRL_SAFE,
	CTRL_ARMING,
	CTRL_ARMED,
	CTRL_FIRING,
};

enum msg_level {
	MSG_INFO,   /* normal progress */
	MSG_WARN,   /* request refused or degraded, nothing unsafe happened */
	MSG_FAULT,  /* something went wrong during arm/fire, demands attention */
};

/* Commanded valve state of an armed hybrid pad */
enum hybrid_op {
	HYB_CLOSED,   /* fill closed, vent closed */
	HYB_FILLING,  /* fill open, vent closed */
	HYB_VENTING,  /* fill closed, vent open */
};

struct sat_view {
	uint64_t uid;   /* the paired satellite, 0 if none */
	bool joined;    /* secure session up */
	bool have_status;
	bool link_ok;   /* status received within the link timeout */
	/*
	 * Its pads are shown and selectable. Drops with link_ok, but only
	 * returns once the link has been stable for a while, so pads do not
	 * flicker at the edge of the radio range.
	 */
	bool visible;
	int64_t ok_since;
	struct sat_status st;
};

struct pad_view {
	/* Channel with a continuity problem, as text ("IGN OPEN"), or NULL */
	const char *problem;
	enum hybrid_op op;
	int64_t fill_start;
};

/* Characters offered by the label editor, in turning order */
#define LABEL_CHARSET " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-./#"

enum setup_field {
	FIELD_LABEL,
	FIELD_TYPE,
	FIELD_SAT,
	FIELD_IGN,
	FIELD_FILL,
	FIELD_VENT,
	FIELD_COUNT,
};

enum setup_mode {
	SETUP_FIELDS,  /* up/down: field, left/right: its value */
	SETUP_LABEL,   /* up/down: character, left/right: position */
};

/* Pad setup screen */
struct pad_setup {
	bool active;
	enum setup_mode mode;
	int pad;
	int field;
	int pos;                          /* label character being edited */
	char label[PAD_LABEL_MAX + 1];    /* being edited, space padded; BACK drops it */
	struct pad_cfg cfg;               /* draft */
	const char *error;                /* why the draft cannot be saved */
};

/* A list with a cursor: a menu, or the pad list */
struct menu_view {
	bool active;
	int cursor;
};

/* Settings menu entries, top to bottom */
enum settings_entry {
	SETTINGS_PADS,
	SETTINGS_SATS,
	SETTINGS_DIAG,
	SETTINGS_OFF,
	SETTINGS_COUNT,
};

/* Diagnostics menu entries, and the screen they open */
enum diag_entry {
	DIAG_BUTTONS,
	DIAG_BATTERY,
	DIAG_COUNT,
	DIAG_NONE = -1,
};

/* Satellites screen, opened from the settings screen */
struct sats_view {
	bool active;
	int cursor;
};

enum pair_phase {
	PAIR_SCAN,       /* listing satellites in pairing mode */
	PAIR_WAIT,       /* key exchange running */
	PAIR_COMPARE,    /* pattern shown, waiting for the operator */
	PAIR_CONFIRMED,  /* operator confirmed, waiting for the satellite's */
};

#define PAIR_MAX_FOUND 4

/*
 * Pairing one satellite slot, see design/radio_security.md. The operator
 * compares the pattern with the satellite's channel LEDs and confirms on
 * both devices.
 */
struct pair_view {
	bool active;
	enum pair_phase phase;
	int sat;
	int found;
	int cursor;
	uint64_t uid[PAIR_MAX_FOUND];
	int16_t rssi_dbm[PAIR_MAX_FOUND];
	uint8_t sas[LSEC_SAS_FRAMES];
};

/* Rows of the armed continuity table visible at once; up and down scroll */
#define ARMED_TABLE_ROWS 5

struct ctrl_view {
	enum ctrl_state state;
	struct sat_view sat[NUM_SATS];
	struct pad_view pad[NUM_PADS];
	uint16_t selected;        /* bit per pad */
	int cursor;               /* pad under the list cursor, never an OFF pad */
	int table_scroll;         /* first visible row of the armed table */
	struct menu_view settings;    /* settings menu, opened by holding OK */
	struct menu_view pad_list;    /* all pads, from the settings menu */
	struct pad_setup setup;       /* opened from the pad list */
	struct sats_view sats;        /* from the settings menu */
	struct pair_view pair;        /* opened from the satellites screen */
	struct menu_view diag_menu;   /* from the settings menu */
	int diag;                     /* open diagnostics screen, DIAG_NONE if none */
	bool power_confirm;       /* asking whether to switch off */
	bool powering_off;        /* switched off, waiting for the power to go */
	bool key_on;
	/* Why arming is currently not possible, NULL if it is */
	const char *arm_block;
	/* Why launching is currently not possible while armed, NULL if it is */
	const char *launch_block;
	int32_t arm_remaining_ms;
	bool arm_timer_paused;    /* while filling */
	int32_t fire_hold_ms;
	char message[80];
	enum msg_level message_level;
	int64_t message_time;
	bool fault_active;        /* the last fault still needs a BACK press */
	uint8_t channel_util;     /* % busy, every network counted */
	uint8_t other_nets;       /* other controllers' networks heard */
	bool channel_crowded;     /* commands may take several tries */
};

void ctrl_tick(void);
const struct ctrl_view *ctrl_view(void);
const char *ctrl_state_name(enum ctrl_state state);

/* Shown in the pad list and selectable: enabled and its satellite visible */
bool ctrl_pad_visible(int p);

enum ctrl_req_type {
	CTRL_REQ_TOGGLE_PAD,  /* arg: pad */
	CTRL_REQ_CURSOR,      /* up/down, arg: rows to move, negative is up */
	CTRL_REQ_ADJUST,      /* left/right, arg: steps, negative is left */
	CTRL_REQ_SELECT,      /* OK: toggle the pad under the cursor */
	CTRL_REQ_EDIT,        /* OK held: settings, or save in pad setup */
	CTRL_REQ_ARM,
	CTRL_REQ_BACK,        /* disarm, acknowledge, back out */
	CTRL_REQ_BACK_LONG,   /* BACK held: leaves the button test */
	CTRL_REQ_FILL,
	CTRL_REQ_VENT,
	CTRL_REQ_POWER,       /* power button: ask to switch off */
};

/* Thread-safe; the request is executed in the next ctrl_tick(). */
void ctrl_post(enum ctrl_req_type type, int arg);

#endif /* CONTROLLER_H_ */
