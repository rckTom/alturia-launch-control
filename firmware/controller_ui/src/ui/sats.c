/*
 * Satellites: one row per satellite slot with its paired satellite, link
 * state and battery. OK pairs, holding OK forgets. The line under them is
 * the latest message for a while, otherwise what the buttons do.
 *
 * Pairing: first the satellites heard in pairing mode, then the 4x6
 * pattern the operator compares with the satellite's channel LEDs:
 *
 *   1  O  #  #  O  O  #     Satellite LEDs show these
 *   2  #  O  O  #  O  O     rows in turn, row 1 with
 *   3  ...                  the status LED lit.
 *   4  ...
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "views.h"
#include "widgets.h"

/* Satellites: all slots fit, LIST_ROWS of them */
BUILD_ASSERT(NUM_SATS <= LIST_ROWS);

static const char sats_help[] = LV_SYMBOL_UP LV_SYMBOL_DOWN
	" satellite   OK: pair   Hold OK: forget   BACK: back";

static lv_obj_t *sats_view;
static lv_obj_t *sats_row[NUM_SATS];
static lv_obj_t *sats_uid[NUM_SATS];
static lv_obj_t *sats_state[NUM_SATS];
static lv_obj_t *sats_volt[NUM_SATS];
static lv_obj_t *sats_footer;

/* Pairing */
#define PAIR_LIST_Y0 84
#define PAIR_LIST_H  26
#define DOT_X0       44
#define DOT_Y0       44
#define DOT_PITCH    32
#define DOT_SIZE     24

static lv_obj_t *pair_view;
static lv_obj_t *pair_title;
static lv_obj_t *pair_text;
static lv_obj_t *pair_found[PAIR_MAX_FOUND];
static lv_obj_t *pair_found_text[PAIR_MAX_FOUND];
static lv_obj_t *pair_grid;
static lv_obj_t *pair_dot[LSEC_SAS_FRAMES][LSEC_SAS_LEDS];
static lv_obj_t *pair_help;

static void create_sats(lv_obj_t *scr)
{
	sats_view = ui_view(scr, false);
	ui_title(sats_view, "MENU  -  SATELLITES");

	for (int sat = 0; sat < NUM_SATS; sat++) {
		lv_obj_t *name;

		sats_row[sat] = ui_box(sats_view, 6, LIST_Y0 + sat * LIST_ROW_H, LIST_ROW_W,
				       LIST_ROW_H - 1);

		name = ui_label(sats_row[sat], FONT_MEDIUM);
		ui_set_text_fmt(name, "%c", sat_name(sat));
		lv_obj_align(name, LV_ALIGN_LEFT_MID, 6, 0);

		sats_uid[sat] = ui_label(sats_row[sat], FONT_SMALL);
		lv_obj_align(sats_uid[sat], LV_ALIGN_LEFT_MID, 32, 0);

		sats_state[sat] = ui_label(sats_row[sat], FONT_SMALL);
		lv_obj_align(sats_state[sat], LV_ALIGN_LEFT_MID, 200, 0);

		sats_volt[sat] = ui_label(sats_row[sat], FONT_SMALL);
		lv_obj_align(sats_volt[sat], LV_ALIGN_RIGHT_MID, -6, 0);
	}

	ui_rule(sats_view, LIST_RULE_Y);
	sats_footer = ui_text_line(sats_view, FONT_SMALL, SCREEN_W - 2 * MARGIN,
				   LV_TEXT_ALIGN_LEFT);
	lv_obj_set_pos(sats_footer, MARGIN, LIST_RULE_Y + 6);
}

static void create_pair(lv_obj_t *scr)
{
	pair_view = ui_view(scr, false);
	pair_title = ui_title(pair_view, "");

	pair_text = ui_label(pair_view, FONT_SMALL);
	lv_label_set_long_mode(pair_text, LV_LABEL_LONG_MODE_WRAP);

	/* Satellites heard in pairing mode */
	for (int i = 0; i < PAIR_MAX_FOUND; i++) {
		pair_found[i] = ui_box(pair_view, ROW_X, PAIR_LIST_Y0 + i * PAIR_LIST_H,
				       SCREEN_W - 36, PAIR_LIST_H - 2);
		pair_found_text[i] = ui_label(pair_found[i], FONT_MEDIUM);
		lv_obj_align(pair_found_text[i], LV_ALIGN_LEFT_MID, 8, 0);
	}

	/* The pattern: a numbered row per LED frame, a dot per channel LED */
	pair_grid = ui_box(pair_view, 0, 0, 240, SCREEN_H - 30);
	for (int r = 0; r < LSEC_SAS_FRAMES; r++) {
		lv_obj_t *num = ui_label(pair_grid, FONT_MEDIUM);

		ui_set_text_fmt(num, "%d", r + 1);
		lv_obj_set_pos(num, 14, DOT_Y0 + r * DOT_PITCH + 1);

		for (int led = 0; led < LSEC_SAS_LEDS; led++) {
			lv_obj_t *dot = ui_box(pair_grid, DOT_X0 + led * DOT_PITCH,
					       DOT_Y0 + r * DOT_PITCH, DOT_SIZE, DOT_SIZE);

			lv_obj_add_style(dot, &ui_style_round, 0);
			lv_obj_add_style(dot, &ui_style_frame, 0);
			pair_dot[r][led] = dot;
		}
	}

	pair_help = ui_footer(pair_view, SCREEN_H - 30, "");
}

void ui_sats_create(lv_obj_t *scr)
{
	create_sats(scr);
	create_pair(scr);
}

/* 16 hex digits; LVGL's formatter is not trusted with 64-bit values */
static void uid_text(char *buf, size_t len, uint64_t uid)
{
	snprintf(buf, len, "%08X%08X", (unsigned int)(uid >> 32), (unsigned int)uid);
}

lv_obj_t *ui_sats_refresh(const struct ctrl_view *v, int64_t now)
{
	char buf[24];

	for (int sat = 0; sat < NUM_SATS; sat++) {
		const struct sat_view *s = &v->sat[sat];

		ui_set_inverted(sats_row[sat], sat == v->sats.cursor);
		if (!s->uid) {
			ui_set_text(sats_uid[sat], "not paired");
			ui_set_text(sats_state[sat], "");
			ui_set_text(sats_volt[sat], "");
			continue;
		}
		uid_text(buf, sizeof(buf), s->uid);
		ui_set_text(sats_uid[sat], buf);
		ui_set_text(sats_state[sat], s->link_ok ? "connected"
					     : s->joined ? "joined, no status"
							 : "no link");
		if (s->have_status) {
			ui_set_text_fmt(sats_volt[sat], "%d.%dV", s->st.vbat_mv / 1000,
					(s->st.vbat_mv % 1000) / 100);
		} else {
			ui_set_text(sats_volt[sat], "");
		}
	}

	if (ui_message_recent(v, now)) {
		ui_set_message(sats_footer, v);
	} else {
		ui_set_text(sats_footer, sats_help);
	}
	return sats_view;
}

/* While scanning: the satellites heard, with the cursor on one */
static void refresh_found(const struct pair_view *pv)
{
	bool scanning = pv->phase == PAIR_SCAN;
	char buf[24];

	for (int i = 0; i < PAIR_MAX_FOUND; i++) {
		bool shown = scanning && i < pv->found;

		ui_set_visible(pair_found[i], shown);
		if (shown) {
			uid_text(buf, sizeof(buf), pv->uid[i]);
			ui_set_text_fmt(pair_found_text[i], "%s   %d dBm", buf, pv->rssi_dbm[i]);
			ui_set_inverted(pair_found[i], i == pv->cursor);
		}
	}
}

/* While comparing: the pattern on the left, the text beside it */
static void refresh_pattern(const struct pair_view *pv)
{
	bool compare = pv->phase == PAIR_COMPARE || pv->phase == PAIR_CONFIRMED;

	ui_set_visible(pair_grid, compare);
	if (compare) {
		for (int r = 0; r < LSEC_SAS_FRAMES; r++) {
			for (int led = 0; led < LSEC_SAS_LEDS; led++) {
				ui_set_inverted(pair_dot[r][led], pv->sas[r] & BIT(led));
			}
		}
		lv_obj_set_pos(pair_text, 250, 40);
		lv_obj_set_width(pair_text, SCREEN_W - 258);
	} else {
		lv_obj_set_pos(pair_text, MARGIN, 36);
		lv_obj_set_width(pair_text, SCREEN_W - 2 * MARGIN);
	}
}

lv_obj_t *ui_pair_refresh(const struct ctrl_view *v)
{
	const struct pair_view *pv = &v->pair;
	char buf[24];

	ui_set_text_fmt(pair_title, "PAIR SATELLITE %c", sat_name(pv->sat));
	refresh_found(pv);
	refresh_pattern(pv);

	switch (pv->phase) {
	case PAIR_SCAN:
		ui_set_text(pair_text,
			    pv->found ? "Satellites in pairing mode. Pick the one with its "
					"status LED blinking fast."
				      : "Hold the pairing button on the satellite for 5 s. "
					"Looking for it...");
		ui_set_text(pair_help, LV_SYMBOL_UP LV_SYMBOL_DOWN " choose   OK: pair   BACK: cancel");
		break;
	case PAIR_WAIT:
		uid_text(buf, sizeof(buf), pv->uid[pv->cursor]);
		ui_set_text_fmt(pair_text, "Exchanging keys with %s...", buf);
		ui_set_text(pair_help, "BACK: cancel");
		break;
	case PAIR_COMPARE:
		ui_set_text(pair_text, "The satellite's channel LEDs show these rows in "
				       "turn, row 1 with the status LED lit.\n\n"
				       "Do they match?");
		ui_set_text(pair_help, "OK: they match   BACK: they differ");
		break;
	case PAIR_CONFIRMED:
		ui_set_text(pair_text, "Now press the button on the satellite.\n\n"
				       "If its LEDs differ, hold it instead.");
		ui_set_text(pair_help, "BACK: cancel");
		break;
	}
	return pair_view;
}
