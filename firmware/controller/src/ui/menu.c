/*
 * Menus: the menu that holding OK opens, of the screens in pads.c,
 * sats.c, diag.c, the messages and the power off dialog, with a line of
 * state each. Diagnostics is a menu of the pages in diag.c.
 *
 * Messages: the last events, newest first, with how long ago and a mark
 * on warnings and faults. Up and down scroll.
 */

#include <stdio.h>

#include "battery.h"
#include "views.h"
#include "widgets.h"

#define MENU_Y0  33
#define MENU_H   28
#define MENU_MAX SETTINGS_COUNT

struct menu_ui {
	lv_obj_t *view;
	lv_obj_t *row[MENU_MAX];
	lv_obj_t *detail[MENU_MAX];
	int count;
};

static struct menu_ui settings_menu;
static struct menu_ui diag_menu;

/* Messages */
static lv_obj_t *msgs_view;
static lv_obj_t *msgs_age[MESSAGES_ROWS];
static lv_obj_t *msgs_mark[MESSAGES_ROWS];
static lv_obj_t *msgs_text[MESSAGES_ROWS];
static lv_obj_t *msgs_bar;
static lv_obj_t *msgs_empty;

static void create_menu(lv_obj_t *scr, struct menu_ui *m, const char *title,
			const char *const *names, int count)
{
	m->view = ui_view(scr, false);
	m->count = count;
	ui_title(m->view, title);

	for (int i = 0; i < count; i++) {
		lv_obj_t *name;

		m->row[i] = ui_box(m->view, 6, MENU_Y0 + i * MENU_H, SCREEN_W - 12, MENU_H - 3);

		name = ui_label(m->row[i], FONT_MEDIUM);
		ui_set_text(name, names[i]);
		lv_obj_align(name, LV_ALIGN_LEFT_MID, 10, 0);

		/* A line of state on the right */
		m->detail[i] = ui_label(m->row[i], FONT_SMALL);
		lv_obj_align(m->detail[i], LV_ALIGN_RIGHT_MID, -10, 0);
	}

	ui_footer(m->view, SCREEN_H - 30, LV_SYMBOL_UP LV_SYMBOL_DOWN " choose    OK: open    BACK: back");
}

static lv_obj_t *refresh_menu(struct menu_ui *m, const struct menu_view *mv)
{
	for (int i = 0; i < m->count; i++) {
		ui_set_inverted(m->row[i], i == mv->cursor);
	}
	return m->view;
}

static void create_messages(lv_obj_t *scr)
{
	msgs_view = ui_view(scr, false);
	ui_title(msgs_view, "MENU  -  MESSAGES");

	for (int i = 0; i < MESSAGES_ROWS; i++) {
		int32_t y = LIST_Y0 + i * LIST_ROW_H;
		lv_obj_t *bang;

		msgs_age[i] = ui_text_line(msgs_view, FONT_SMALL, 44, LV_TEXT_ALIGN_RIGHT);
		lv_obj_set_pos(msgs_age[i], 4, y + 1);

		/* Warnings and faults: a black box with "!" */
		msgs_mark[i] = ui_box(msgs_view, 54, y + 2, 14, LIST_ROW_H - 4);
		ui_set_inverted(msgs_mark[i], true);
		bang = ui_label(msgs_mark[i], FONT_SMALL);
		ui_set_text(bang, "!");
		lv_obj_center(bang);

		msgs_text[i] = ui_text_line(msgs_view, FONT_SMALL, LIST_ROW_W - 74,
					    LV_TEXT_ALIGN_LEFT);
		lv_obj_set_pos(msgs_text[i], 74, y + 1);
	}
	msgs_bar = ui_scrollbar(msgs_view, LIST_Y0, LIST_H);

	msgs_empty = ui_text_line(msgs_view, FONT_MEDIUM, SCREEN_W - 2 * MARGIN,
				  LV_TEXT_ALIGN_CENTER);
	lv_obj_set_pos(msgs_empty, MARGIN, LIST_Y0 + LIST_H / 2 - 12);
	ui_set_text(msgs_empty, "No messages yet");

	ui_footer(msgs_view, LIST_RULE_Y, LV_SYMBOL_UP LV_SYMBOL_DOWN " scroll    BACK: back");
}

void ui_menus_create(lv_obj_t *scr)
{
	static const char *const settings_names[SETTINGS_COUNT] = {
		[SETTINGS_EDIT] = "Edit pad",
		[SETTINGS_PADS] = "Pads",
		[SETTINGS_SATS] = "Satellites",
		[SETTINGS_MSGS] = "Messages",
		[SETTINGS_DIAG] = "Diagnostics",
		[SETTINGS_OFF] = "Power off",
	};
	static const char *const diag_names[DIAG_COUNT] = {
		[DIAG_BUTTONS] = "Buttons",
		[DIAG_BATTERY] = "Battery",
		[DIAG_IGNLOG] = "Ignition log",
		[DIAG_GAME] = "Stacker",
	};

	create_menu(scr, &settings_menu, "MENU", settings_names, SETTINGS_COUNT);
	create_menu(scr, &diag_menu, "MENU  -  DIAGNOSTICS", diag_names, DIAG_COUNT);
	create_messages(scr);
}

lv_obj_t *ui_settings_refresh(const struct ctrl_view *v)
{
	const struct ctrl_event *latest = ctrl_event(0);
	int used = 0;
	int paired = 0;

	for (int p = 0; p < NUM_PADS; p++) {
		used += pads[p].type != PAD_OFF;
	}
	for (int sat = 0; sat < NUM_SATS; sat++) {
		paired += v->sat[sat].uid != 0;
	}
	ui_set_text(settings_menu.detail[SETTINGS_EDIT],
		    ctrl_pad_listed(v->cursor) ? pad_name(v->cursor) : "no pad selected");
	ui_set_text_fmt(settings_menu.detail[SETTINGS_PADS], "%d set up", used);
	ui_set_text_fmt(settings_menu.detail[SETTINGS_SATS], "%d of %d paired", paired, NUM_SATS);
	ui_set_text(settings_menu.detail[SETTINGS_MSGS],
		    latest && latest->level != MSG_INFO ? "warnings" : "");
	return refresh_menu(&settings_menu, &v->settings);
}

lv_obj_t *ui_diag_menu_refresh(const struct ctrl_view *v)
{
	struct battery_info b;

	battery_get(&b);
	if (b.gauge) {
		ui_set_text_fmt(diag_menu.detail[DIAG_BATTERY], "%u %%%s", b.soc_pct,
				battery_charging(&b) ? ", charging" : "");
	} else {
		ui_set_text(diag_menu.detail[DIAG_BATTERY], "no reading");
	}
	ui_set_text(diag_menu.detail[DIAG_IGNLOG], "igniter current");
	ui_set_text(diag_menu.detail[DIAG_GAME], "a game");
	return refresh_menu(&diag_menu, &v->diag_menu);
}

/* "12s", "5m", "2h": enough to tell this launch's events from older ones */
static void age_text(lv_obj_t *label, int64_t ms)
{
	int64_t s = ms / 1000;

	if (s < 60) {
		ui_set_text_fmt(label, "%ds", (int)s);
	} else if (s < 3600) {
		ui_set_text_fmt(label, "%dm", (int)(s / 60));
	} else {
		ui_set_text_fmt(label, "%dh", (int)(s / 3600));
	}
}

lv_obj_t *ui_messages_refresh(const struct ctrl_view *v, int64_t now)
{
	int count = 0;

	while (ctrl_event(count)) {
		count++;
	}
	for (int i = 0; i < MESSAGES_ROWS; i++) {
		const struct ctrl_event *e = ctrl_event(v->messages.cursor + i);

		ui_set_visible(msgs_age[i], e != NULL);
		ui_set_visible(msgs_mark[i], e && e->level != MSG_INFO);
		ui_set_visible(msgs_text[i], e != NULL);
		if (e) {
			age_text(msgs_age[i], now - e->time);
			ui_set_text(msgs_text[i], e->text);
		}
	}
	ui_scrollbar_set(msgs_bar, LIST_Y0, LIST_H, v->messages.cursor, count, MESSAGES_ROWS);
	ui_set_visible(msgs_empty, count == 0);
	return msgs_view;
}
