/*
 * Menus: Settings is a menu of the screens in pads.c, sats.c, diag.c and
 * the power off dialog, with a line of state each. Diagnostics is a menu
 * of the pages in diag.c.
 */

#include "battery.h"
#include "views.h"
#include "widgets.h"

#define MENU_Y0  34
#define MENU_H   32
#define MENU_MAX 4

struct menu_ui {
	lv_obj_t *view;
	lv_obj_t *row[MENU_MAX];
	lv_obj_t *detail[MENU_MAX];
	lv_obj_t *cursor;
	int count;
};

static struct menu_ui settings_menu;
static struct menu_ui diag_menu;

static void create_menu(lv_obj_t *scr, struct menu_ui *m, const char *title,
			const char *const *names, int count)
{
	m->view = ui_view(scr, false);
	m->count = count;
	ui_title(m->view, title);

	for (int i = 0; i < count; i++) {
		lv_obj_t *name;

		m->row[i] = ui_box(m->view, ROW_X, MENU_Y0 + i * MENU_H, SCREEN_W - 36,
				   MENU_H - 4);

		name = ui_label(m->row[i], FONT_MEDIUM);
		ui_set_text(name, names[i]);
		lv_obj_align(name, LV_ALIGN_LEFT_MID, 10, 0);

		/* A line of state on the right */
		m->detail[i] = ui_label(m->row[i], FONT_SMALL);
		lv_obj_align(m->detail[i], LV_ALIGN_RIGHT_MID, -10, 0);
	}
	m->cursor = ui_cursor(m->view);

	ui_footer(m->view, SCREEN_H - 30, LV_SYMBOL_UP LV_SYMBOL_DOWN " choose    OK: open    BACK: back");
}

static lv_obj_t *refresh_menu(struct menu_ui *m, const struct menu_view *mv)
{
	for (int i = 0; i < m->count; i++) {
		ui_set_inverted(m->row[i], i == mv->cursor);
	}
	lv_obj_set_pos(m->cursor, CURSOR_X, MENU_Y0 + mv->cursor * MENU_H + 5);
	return m->view;
}

void ui_menus_create(lv_obj_t *scr)
{
	static const char *const settings_names[SETTINGS_COUNT] = {
		[SETTINGS_PADS] = "Pads",
		[SETTINGS_SATS] = "Satellites",
		[SETTINGS_DIAG] = "Diagnostics",
		[SETTINGS_OFF] = "Power off",
	};
	static const char *const diag_names[DIAG_COUNT] = {
		[DIAG_BUTTONS] = "Buttons",
		[DIAG_BATTERY] = "Battery",
	};

	create_menu(scr, &settings_menu, "SETTINGS", settings_names, SETTINGS_COUNT);
	create_menu(scr, &diag_menu, "SETTINGS  -  DIAGNOSTICS", diag_names, DIAG_COUNT);
}

lv_obj_t *ui_settings_refresh(const struct ctrl_view *v)
{
	int used = 0;
	int paired = 0;

	for (int p = 0; p < NUM_PADS; p++) {
		used += pads[p].type != PAD_OFF;
	}
	for (int sat = 0; sat < NUM_SATS; sat++) {
		paired += v->sat[sat].uid != 0;
	}
	ui_set_text_fmt(settings_menu.detail[SETTINGS_PADS], "%d of %d in use", used, NUM_PADS);
	ui_set_text_fmt(settings_menu.detail[SETTINGS_SATS], "%d of %d paired", paired, NUM_SATS);
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
	return refresh_menu(&diag_menu, &v->diag_menu);
}
