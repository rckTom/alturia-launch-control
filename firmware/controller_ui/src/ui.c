/*
 * 400x240 1-bit status display (Sharp Memory LCD class). There is no
 * touch: all input comes from the physical controls. Only black and
 * white exist, so emphasis is done with inversion, never with color.
 *
 * List view (SAFE, ARMING):
 *  +------------------------------------------------+
 *  | SAFE               [A  12.4V]  [#B##NO#LINK#]  |
 *  |------------------------------------------------|
 *  |   ALFA     HYB   A        (only pads of        |
 *  | > #Pad 2###RKT###A#######connected satellites)#| <- cursor, selected
 *  | ...                                            |
 *  |------------------------------------------------|
 *  | next step, or why arming is blocked            |
 *  +------------------------------------------------+
 *
 * Armed view (ARMED, FIRING): whole screen inverted. The arm timeout and a
 * continuity table of the armed pads (problems inverted again), valve
 * state of hybrid pads and the fire hold progress:
 *
 *  ARMED    [! CONTINUITY: 1 PAD]           0:42
 *  PAD      IGN    FILL   VENT   VALVES
 *  ALFA     OK     OK     OK     FILL 0:12
 *  Pad 2    OPEN
 *
 * Rows are sorted: continuity problems first, then hybrid pads (for their
 * valve state), then the rest. The encoder scrolls when they do not fit.
 *
 * Continuity is only shown here: the satellite reads it from the armed
 * rail. Before arming it only runs its own shorted-output check.
 *
 * Fault view: one sentence in a heavy frame, until SAFE acknowledges it.
 *
 * Settings: all pads including OFF ones, with their channels. The main
 * list only shows enabled pads.
 *
 * Pad setup: label, type, satellite and channels of one pad. The focused
 * field is inverted; the label is edited in one box per character.
 */

#include <stdio.h>

#include <zephyr/kernel.h>
#include <lvgl.h>

#include "controller.h"
#include "ui.h"

#define W 400
#define H 240

#define MESSAGE_SHOW_MS 5000

#define ROW_Y0 31
#define ROW_H  24

struct row {
	lv_obj_t *box;
	lv_obj_t *name;
	lv_obj_t *type;
	lv_obj_t *sat;
};

/* List view */
static lv_obj_t *list_view;
static lv_obj_t *state_label;
static lv_obj_t *sat_box[NUM_SATS];
static lv_obj_t *sat_text[NUM_SATS];
static struct row rows[NUM_PADS];
static lv_obj_t *cursor_label;
static lv_obj_t *footer_label;

/* Armed view */
static lv_obj_t *armed_view;
enum { TCOL_PAD, TCOL_IGN, TCOL_FILL, TCOL_VENT, TCOL_VALVES, TCOL_COUNT };

#define TABLE_Y0 34
#define TABLE_H  20

static const int32_t tcol_x[TCOL_COUNT] = {6, 96, 152, 208, 268};
static const int32_t tcol_w[TCOL_COUNT] = {86, 52, 52, 56, 124};

static lv_obj_t *armed_title;
static lv_obj_t *armed_warn_box;
static lv_obj_t *armed_warn;
static lv_obj_t *armed_timer;
static lv_obj_t *armed_big;
static lv_obj_t *armed_table;
static lv_obj_t *table_box[ARMED_TABLE_ROWS][TCOL_COUNT];
static lv_obj_t *table_text[ARMED_TABLE_ROWS][TCOL_COUNT];
static lv_obj_t *table_summary;
static lv_obj_t *armed_hint;
static lv_obj_t *fire_bar;

/* Fault view */
static lv_obj_t *fault_view;
static lv_obj_t *fault_text;

/* Settings */
static lv_obj_t *settings_view;
enum { SCOL_NUM, SCOL_LABEL, SCOL_TYPE, SCOL_SAT, SCOL_CHANNELS, SCOL_COUNT };

static const int32_t settings_col_x[SCOL_COUNT] = {6, 28, 90, 160, 184};

static lv_obj_t *settings_rows[NUM_PADS];
static lv_obj_t *settings_col[NUM_PADS][SCOL_COUNT];
static lv_obj_t *settings_cursor;

/* Pad setup */
#define SETUP_Y0 33
#define SETUP_H  26

static const char *const field_names[FIELD_COUNT] = {
	"Label", "Type", "Satellite", "Igniter", "Fill valve", "Vent valve",
};

static lv_obj_t *setup_view;
static lv_obj_t *setup_title;
static lv_obj_t *setup_cursor;
static lv_obj_t *setup_row[FIELD_COUNT];
static lv_obj_t *setup_value[FIELD_COUNT];
static lv_obj_t *setup_char_box[PAD_LABEL_MAX];
static lv_obj_t *setup_char[PAD_LABEL_MAX];
static lv_obj_t *setup_help;
static lv_obj_t *setup_error;

static lv_color_t fg(bool inverted)
{
	return inverted ? lv_color_white() : lv_color_black();
}

static lv_color_t bg(bool inverted)
{
	return inverted ? lv_color_black() : lv_color_white();
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, font, 0);
	lv_label_set_text(l, "");
	return l;
}

static lv_obj_t *make_box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
	lv_obj_t *box = lv_obj_create(parent);

	lv_obj_set_pos(box, x, y);
	lv_obj_set_size(box, w, h);
	lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(box, 0, 0);
	lv_obj_set_style_radius(box, 0, 0);
	lv_obj_set_style_pad_all(box, 0, 0);
	lv_obj_set_style_shadow_width(box, 0, 0);
	lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
	return box;
}

static void make_rule(lv_obj_t *parent, int32_t y)
{
	lv_obj_t *rule = make_box(parent, 6, y, W - 12, 1);

	lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
}

static void set_inverted(lv_obj_t *obj, bool inverted)
{
	lv_obj_set_style_bg_color(obj, bg(inverted), 0);
	lv_obj_set_style_text_color(obj, fg(inverted), 0);
}

static void show_only(lv_obj_t *view)
{
	lv_obj_t *views[] = {list_view, armed_view, fault_view, settings_view, setup_view};

	ARRAY_FOR_EACH(views, i) {
		if (views[i] == view) {
			lv_obj_remove_flag(views[i], LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(views[i], LV_OBJ_FLAG_HIDDEN);
		}
	}
}

static void create_list_view(lv_obj_t *scr)
{
	list_view = make_box(scr, 0, 0, W, H);
	set_inverted(list_view, false);

	state_label = make_label(list_view, &lv_font_montserrat_16);
	lv_obj_set_pos(state_label, 8, 5);

	/* One box per satellite, inverted when it needs attention */
	for (int sat = 0; sat < NUM_SATS; sat++) {
		sat_box[sat] = make_box(list_view, W - 6 - (NUM_SATS - sat) * 108, 3, 104, 21);
		sat_text[sat] = make_label(sat_box[sat], &lv_font_montserrat_14);
		lv_obj_center(sat_text[sat]);
	}

	make_rule(list_view, 27);

	for (int p = 0; p < NUM_PADS; p++) {
		struct row *r = &rows[p];

		r->box = make_box(list_view, 18, ROW_Y0 + p * ROW_H, W - 22, ROW_H - 2);

		r->name = make_label(r->box, &lv_font_montserrat_16);
		lv_obj_set_width(r->name, 88);
		lv_label_set_long_mode(r->name, LV_LABEL_LONG_MODE_CLIP);
		lv_obj_align(r->name, LV_ALIGN_LEFT_MID, 6, 0);

		r->type = make_label(r->box, &lv_font_montserrat_14);
		lv_obj_align(r->type, LV_ALIGN_LEFT_MID, 100, 0);

		r->sat = make_label(r->box, &lv_font_montserrat_14);
		lv_obj_align(r->sat, LV_ALIGN_LEFT_MID, 142, 0);

	}

	/* Outside the rows, so it reads on selected (inverted) rows too */
	cursor_label = make_label(list_view, &lv_font_montserrat_14);
	lv_label_set_text(cursor_label, LV_SYMBOL_RIGHT);

	make_rule(list_view, ROW_Y0 + NUM_PADS * ROW_H + 1);

	footer_label = make_label(list_view, &lv_font_montserrat_14);
	lv_obj_set_pos(footer_label, 8, ROW_Y0 + NUM_PADS * ROW_H + 9);
	lv_obj_set_width(footer_label, W - 16);
	lv_label_set_long_mode(footer_label, LV_LABEL_LONG_MODE_DOTS);
}

static void create_armed_view(lv_obj_t *scr)
{
	static const char *const heads[TCOL_COUNT] = {"PAD", "IGN", "FILL", "VENT", "VALVES"};

	armed_view = make_box(scr, 0, 0, W, H);
	set_inverted(armed_view, true);

	armed_title = make_label(armed_view, &lv_font_montserrat_16);
	lv_obj_set_pos(armed_title, 8, 6);

	armed_timer = make_label(armed_view, &lv_font_montserrat_24);
	lv_obj_align(armed_timer, LV_ALIGN_TOP_RIGHT, -8, 2);

	/* Continuity warning in the title bar, visible whatever is scrolled */
	armed_warn_box = make_box(armed_view, 96, 4, 200, 24);
	set_inverted(armed_warn_box, false);
	armed_warn = make_label(armed_warn_box, &lv_font_montserrat_14);
	lv_obj_center(armed_warn);

	/* Only while firing, in place of the table */
	armed_big = make_label(armed_view, &lv_font_montserrat_48);
	lv_obj_set_width(armed_big, W - 16);
	lv_obj_set_style_text_align(armed_big, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(armed_big, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_align(armed_big, LV_ALIGN_TOP_MID, 0, 70);

	/* Continuity table: header plus one row per armed pad */
	armed_table = make_box(armed_view, 0, TABLE_Y0, W, (ARMED_TABLE_ROWS + 2) * TABLE_H);
	set_inverted(armed_table, true);
	for (int c = 0; c < TCOL_COUNT; c++) {
		lv_obj_t *head = make_label(armed_table, &lv_font_montserrat_14);

		lv_label_set_text(head, heads[c]);
		lv_obj_set_pos(head, tcol_x[c] + 3, 2);
	}
	for (int r = 0; r < ARMED_TABLE_ROWS; r++) {
		for (int c = 0; c < TCOL_COUNT; c++) {
			table_box[r][c] = make_box(armed_table, tcol_x[c], (r + 1) * TABLE_H,
						   tcol_w[c], TABLE_H - 2);
			table_text[r][c] = make_label(table_box[r][c], &lv_font_montserrat_14);
			lv_obj_align(table_text[r][c], LV_ALIGN_LEFT_MID, 3, 0);
		}
	}
	/* Scroll indicator below the rows */
	table_summary = make_label(armed_table, &lv_font_montserrat_14);
	lv_obj_set_pos(table_summary, tcol_x[TCOL_PAD] + 3, (ARMED_TABLE_ROWS + 1) * TABLE_H + 2);

	armed_hint = make_label(armed_view, &lv_font_montserrat_14);
	lv_obj_set_width(armed_hint, W - 16);
	lv_obj_set_style_text_align(armed_hint, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(armed_hint, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_align(armed_hint, LV_ALIGN_BOTTOM_MID, 0, -24);

	fire_bar = lv_bar_create(armed_view);
	lv_obj_set_size(fire_bar, 300, 10);
	lv_obj_align(fire_bar, LV_ALIGN_BOTTOM_MID, 0, -8);
	lv_bar_set_range(fire_bar, 0, CONFIG_APP_FIRE_HOLD_MS);
	lv_obj_set_style_radius(fire_bar, 0, LV_PART_MAIN);
	lv_obj_set_style_radius(fire_bar, 0, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(fire_bar, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_bg_color(fire_bar, lv_color_black(), LV_PART_MAIN);
	lv_obj_set_style_border_width(fire_bar, 1, LV_PART_MAIN);
	lv_obj_set_style_border_color(fire_bar, lv_color_white(), LV_PART_MAIN);
	lv_obj_set_style_bg_color(fire_bar, lv_color_white(), LV_PART_INDICATOR);
}

static void create_fault_view(lv_obj_t *scr)
{
	lv_obj_t *title;

	fault_view = make_box(scr, 0, 0, W, H);
	set_inverted(fault_view, false);
	lv_obj_set_style_border_width(fault_view, 8, 0);
	lv_obj_set_style_border_color(fault_view, lv_color_black(), 0);

	title = make_label(fault_view, &lv_font_montserrat_24);
	lv_label_set_text(title, LV_SYMBOL_WARNING "  FAULT");
	lv_obj_align(title, LV_ALIGN_CENTER, 0, -40);

	fault_text = make_label(fault_view, &lv_font_montserrat_16);
	lv_obj_set_width(fault_text, W - 60);
	lv_obj_set_style_text_align(fault_text, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(fault_text, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_align(fault_text, LV_ALIGN_CENTER, 0, 10);

	title = make_label(fault_view, &lv_font_montserrat_14);
	lv_label_set_text(title, "Press SAFE to acknowledge");
	lv_obj_align(title, LV_ALIGN_BOTTOM_MID, 0, -20);
}

static void create_settings_view(lv_obj_t *scr)
{
	lv_obj_t *title;
	lv_obj_t *help;

	settings_view = make_box(scr, 0, 0, W, H);
	set_inverted(settings_view, false);

	title = make_label(settings_view, &lv_font_montserrat_16);
	lv_label_set_text(title, "SETTINGS  -  PADS");
	lv_obj_set_pos(title, 8, 5);
	make_rule(settings_view, 27);

	for (int p = 0; p < NUM_PADS; p++) {
		settings_rows[p] = make_box(settings_view, 18, ROW_Y0 + p * ROW_H, W - 22,
					    ROW_H - 2);
		for (int c = 0; c < SCOL_COUNT; c++) {
			settings_col[p][c] = make_label(settings_rows[p], &lv_font_montserrat_14);
			lv_obj_align(settings_col[p][c], LV_ALIGN_LEFT_MID, settings_col_x[c], 0);
		}
		lv_obj_set_width(settings_col[p][SCOL_LABEL], 60);
		lv_label_set_long_mode(settings_col[p][SCOL_LABEL], LV_LABEL_LONG_MODE_CLIP);
	}

	settings_cursor = make_label(settings_view, &lv_font_montserrat_14);
	lv_label_set_text(settings_cursor, LV_SYMBOL_RIGHT);

	make_rule(settings_view, ROW_Y0 + NUM_PADS * ROW_H + 1);

	help = make_label(settings_view, &lv_font_montserrat_14);
	lv_label_set_text(help, "Turn: pad    Push: set up    SAFE: back");
	lv_obj_set_pos(help, 8, ROW_Y0 + NUM_PADS * ROW_H + 9);
}

static void create_setup_view(lv_obj_t *scr)
{
	setup_view = make_box(scr, 0, 0, W, H);
	set_inverted(setup_view, false);

	setup_title = make_label(setup_view, &lv_font_montserrat_16);
	lv_obj_set_pos(setup_title, 8, 5);
	make_rule(setup_view, 27);

	for (int f = 0; f < FIELD_COUNT; f++) {
		lv_obj_t *name;

		setup_row[f] = make_box(setup_view, 18, SETUP_Y0 + f * SETUP_H, W - 36,
					SETUP_H - 2);

		name = make_label(setup_row[f], &lv_font_montserrat_14);
		lv_label_set_text(name, field_names[f]);
		lv_obj_align(name, LV_ALIGN_LEFT_MID, 8, 0);

		setup_value[f] = make_label(setup_row[f], &lv_font_montserrat_16);
		lv_obj_align(setup_value[f], LV_ALIGN_LEFT_MID, 110, 0);
	}

	/* Character boxes over the label value, shown while editing it */
	for (int i = 0; i < PAD_LABEL_MAX; i++) {
		setup_char_box[i] = make_box(setup_row[FIELD_LABEL], 106 + i * 19, 1, 18,
					     SETUP_H - 4);
		lv_obj_set_style_border_width(setup_char_box[i], 1, 0);
		lv_obj_set_style_border_color(setup_char_box[i], lv_color_black(), 0);
		setup_char[i] = make_label(setup_char_box[i], &lv_font_montserrat_16);
		lv_obj_center(setup_char[i]);
	}

	setup_cursor = make_label(setup_view, &lv_font_montserrat_14);
	lv_label_set_text(setup_cursor, LV_SYMBOL_RIGHT);

	make_rule(setup_view, SETUP_Y0 + FIELD_COUNT * SETUP_H + 1);

	setup_help = make_label(setup_view, &lv_font_montserrat_14);
	lv_obj_set_pos(setup_help, 8, SETUP_Y0 + FIELD_COUNT * SETUP_H + 7);

	setup_error = make_label(setup_view, &lv_font_montserrat_14);
	lv_obj_set_pos(setup_error, 8, SETUP_Y0 + FIELD_COUNT * SETUP_H + 24);
	lv_obj_set_width(setup_error, W - 16);
	lv_label_set_long_mode(setup_error, LV_LABEL_LONG_MODE_DOTS);
}

void ui_init(void)
{
	lv_obj_t *scr = lv_screen_active();

	lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

	create_list_view(scr);
	create_armed_view(scr);
	create_fault_view(scr);
	create_settings_view(scr);
	create_setup_view(scr);

	ui_refresh();
}

static void refresh_row(const struct ctrl_view *v, int slot, int p)
{
	const struct row *r = &rows[slot];

	lv_obj_remove_flag(r->box, LV_OBJ_FLAG_HIDDEN);
	set_inverted(r->box, v->selected & BIT(p));
	lv_label_set_text(r->name, pad_name(p));
	lv_label_set_text(r->type, pads[p].type == PAD_HYBRID ? "HYB" : "RKT");
	lv_label_set_text_fmt(r->sat, "%c", sat_name(pads[p].sat));
}

static void refresh_list_view(const struct ctrl_view *v, int64_t now)
{
	bool any_visible = false;

	for (int sat = 0; sat < NUM_SATS; sat++) {
		const struct sat_view *s = &v->sat[sat];
		/* 3S pack: flag below 3.5 V per cell */
		bool low = s->st.vbat_mv < 10500;

		any_visible |= s->visible;
		if (s->visible) {
			lv_label_set_text_fmt(sat_text[sat], "%c  %d.%dV%s", sat_name(sat),
					      s->st.vbat_mv / 1000, (s->st.vbat_mv % 1000) / 100,
					      low ? " LOW" : "");
			set_inverted(sat_box[sat], low);
		} else if (s->link_ok) {
			lv_label_set_text_fmt(sat_text[sat], "%c  connecting", sat_name(sat));
			set_inverted(sat_box[sat], false);
		} else {
			lv_label_set_text_fmt(sat_text[sat], "%c  NO LINK", sat_name(sat));
			set_inverted(sat_box[sat], true);
		}
	}

	lv_label_set_text(state_label, any_visible || v->state != CTRL_SAFE
				       ? ctrl_state_name(v->state) : "NO LINK");

	/* Only pads that can be used right now, packed from the top */
	int slot = 0;

	lv_obj_add_flag(cursor_label, LV_OBJ_FLAG_HIDDEN);
	for (int p = 0; p < NUM_PADS; p++) {
		if (!ctrl_pad_visible(p)) {
			continue;
		}
		if (p == v->cursor) {
			lv_obj_remove_flag(cursor_label, LV_OBJ_FLAG_HIDDEN);
			lv_obj_set_pos(cursor_label, 3, ROW_Y0 + slot * ROW_H + 3);
		}
		refresh_row(v, slot++, p);
	}
	for (; slot < NUM_PADS; slot++) {
		lv_obj_add_flag(rows[slot].box, LV_OBJ_FLAG_HIDDEN);
	}

	/* Footer: the most recent event, otherwise the next step */
	if (v->message[0] && now - v->message_time < MESSAGE_SHOW_MS) {
		lv_label_set_text_fmt(footer_label, "%s%s",
				      v->message_level == MSG_INFO ? "" : LV_SYMBOL_WARNING " ",
				      v->message);
	} else if (v->state == CTRL_ARMING) {
		lv_label_set_text(footer_label, "Arming...");
	} else if (!any_visible) {
		lv_label_set_text(footer_label, "No satellites connected");
	} else if (lv_obj_has_flag(cursor_label, LV_OBJ_FLAG_HIDDEN)) {
		lv_label_set_text(footer_label, "No pads available. Hold select for settings.");
	} else if (v->arm_block) {
		lv_label_set_text(footer_label, v->arm_block);
	} else {
		char list[PADS_FORMAT_LEN];

		pads_format(list, sizeof(list), v->selected);
		lv_label_set_text_fmt(footer_label, "Press ARM to arm %s", list);
	}
}

static const char *cont_text(const struct sat_view *s, int ch)
{
	if (!s->have_status) {
		return "?";
	}
	switch (s->st.cont[ch]) {
	case CONT_OK:
		return "OK";
	case CONT_SHORT:
		return "SHORT";
	default:
		return "OPEN";
	}
}

static void set_cell(int row, int col, const char *text, bool problem)
{
	lv_label_set_text(table_text[row][col], text);
	/* The view is inverted; problems flip back to black on white */
	set_inverted(table_box[row][col], !problem);
}

static void refresh_table_row(const struct ctrl_view *v, int row, int p, int64_t now)
{
	const struct pad_cfg *cfg = &pads[p];
	const struct sat_view *s = &v->sat[cfg->sat];
	uint8_t valves = s->st.valve_mask;
	char buf[24];

	set_cell(row, TCOL_PAD, pad_name(p), false);
	set_cell(row, TCOL_IGN, cont_text(s, cfg->ign), s->st.cont[cfg->ign] != CONT_OK);

	if (cfg->type != PAD_HYBRID) {
		set_cell(row, TCOL_FILL, "", false);
		set_cell(row, TCOL_VENT, "", false);
		set_cell(row, TCOL_VALVES, "", false);
		return;
	}

	set_cell(row, TCOL_FILL, cont_text(s, cfg->fill), s->st.cont[cfg->fill] != CONT_OK);
	set_cell(row, TCOL_VENT, cont_text(s, cfg->vent), s->st.cont[cfg->vent] != CONT_OK);

	/* Valve state as reported by the satellite, not as commanded */
	if (valves & BIT(cfg->fill)) {
		int32_t t = (int32_t)((now - v->pad[p].fill_start) / 1000);

		snprintf(buf, sizeof(buf), "FILL %d:%02d", t / 60, t % 60);
	} else {
		snprintf(buf, sizeof(buf), "%s", valves & BIT(cfg->vent) ? "CLOSED" : "VENTING");
	}
	set_cell(row, TCOL_VALVES, buf, false);
}

static void refresh_armed_view(const struct ctrl_view *v, int64_t now)
{
	bool hybrid = false;
	char pads_buf[PADS_FORMAT_LEN];
	int row = 0;

	if (v->state == CTRL_FIRING) {
		/* Flash the whole screen */
		pads_format(pads_buf, sizeof(pads_buf), v->selected);
		set_inverted(armed_view, (now / 250) % 2);
		lv_label_set_text(armed_title, "FIRING");
		lv_label_set_text(armed_timer, "");
		lv_obj_add_flag(armed_warn_box, LV_OBJ_FLAG_HIDDEN);
		lv_label_set_text(armed_big, pads_buf);
		lv_obj_remove_flag(armed_big, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(armed_table, LV_OBJ_FLAG_HIDDEN);
		lv_label_set_text(armed_hint, "");
		lv_obj_add_flag(fire_bar, LV_OBJ_FLAG_HIDDEN);
		return;
	}

	set_inverted(armed_view, true);
	lv_obj_add_flag(armed_big, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(armed_table, LV_OBJ_FLAG_HIDDEN);
	lv_label_set_text(armed_title, "ARMED");

	int32_t s = (v->arm_remaining_ms + 999) / 1000;

	lv_label_set_text_fmt(armed_timer, "%d:%02d", s / 60, s % 60);
	lv_obj_align(armed_timer, LV_ALIGN_TOP_RIGHT, -8, 2);

	/* Sort: continuity problems, then hybrids, then the rest */
	int order[NUM_PADS];
	int n = 0;

	for (int rank = 0; rank < 3; rank++) {
		for (int p = 0; p < NUM_PADS; p++) {
			int r;

			if (!(v->selected & BIT(p))) {
				continue;
			}
			r = v->pad[p].problem ? 0 : pads[p].type == PAD_HYBRID ? 1 : 2;
			if (r == rank) {
				order[n++] = p;
			}
			hybrid |= pads[p].type == PAD_HYBRID;
		}
	}

	int problems = 0;

	for (int i = 0; i < n; i++) {
		problems += v->pad[order[i]].problem != NULL;
	}
	if (problems) {
		lv_label_set_text_fmt(armed_warn, LV_SYMBOL_WARNING " CONTINUITY: %d PAD%s",
				      problems, problems == 1 ? "" : "S");
		lv_obj_remove_flag(armed_warn_box, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(armed_warn_box, LV_OBJ_FLAG_HIDDEN);
	}

	int first = MIN(v->table_scroll, MAX(n - ARMED_TABLE_ROWS, 0));

	for (row = 0; row < ARMED_TABLE_ROWS; row++) {
		bool used = first + row < n;

		for (int c = 0; c < TCOL_COUNT; c++) {
			if (used) {
				lv_obj_remove_flag(table_box[row][c], LV_OBJ_FLAG_HIDDEN);
			} else {
				lv_obj_add_flag(table_box[row][c], LV_OBJ_FLAG_HIDDEN);
			}
		}
		if (used) {
			refresh_table_row(v, row, order[first + row], now);
		}
	}

	int above = first;
	int below = MAX(n - first - ARMED_TABLE_ROWS, 0);

	if (above || below) {
		lv_label_set_text_fmt(table_summary, "%s %d above      %s %d below",
				      LV_SYMBOL_UP, above, LV_SYMBOL_DOWN, below);
	} else {
		lv_label_set_text(table_summary, "");
	}

	if (v->launch_block) {
		lv_label_set_text_fmt(armed_hint, LV_SYMBOL_WARNING " %s  -  SAFE aborts",
				      v->launch_block);
	} else if (v->arm_timer_paused) {
		lv_label_set_text(armed_hint, "Arm timer paused while filling  -  SAFE aborts");
	} else {
		lv_label_set_text(armed_hint, hybrid ? "FILL  /  VENT  /  hold LAUNCH  -  SAFE aborts"
						     : "Hold FIRE to launch  -  SAFE aborts");
	}
	lv_obj_remove_flag(fire_bar, LV_OBJ_FLAG_HIDDEN);
	lv_bar_set_value(fire_bar, v->fire_hold_ms, LV_ANIM_OFF);
}

static void refresh_settings_view(const struct ctrl_view *v)
{
	for (int p = 0; p < NUM_PADS; p++) {
		const struct pad_cfg *cfg = &pads[p];
		lv_obj_t **col = settings_col[p];

		lv_label_set_text_fmt(col[SCOL_NUM], "%d", p + 1);
		lv_label_set_text(col[SCOL_LABEL], cfg->label[0] ? cfg->label : "-");
		lv_label_set_text(col[SCOL_TYPE], pad_type_name(cfg->type));

		switch (cfg->type) {
		case PAD_HYBRID:
			lv_label_set_text_fmt(col[SCOL_SAT], "%c", sat_name(cfg->sat));
			lv_label_set_text_fmt(col[SCOL_CHANNELS], "IGN %d  FILL %d  VENT %d",
					      cfg->ign + 1, cfg->fill + 1, cfg->vent + 1);
			break;
		case PAD_ROCKET:
			lv_label_set_text_fmt(col[SCOL_SAT], "%c", sat_name(cfg->sat));
			lv_label_set_text_fmt(col[SCOL_CHANNELS], "IGN %d", cfg->ign + 1);
			break;
		default:
			lv_label_set_text(col[SCOL_SAT], "");
			lv_label_set_text(col[SCOL_CHANNELS], "");
			break;
		}
	}
	lv_obj_set_pos(settings_cursor, 3, ROW_Y0 + v->settings.cursor * ROW_H + 3);
}

static void setup_field_text(const struct pad_cfg *cfg, int field, char *buf, size_t len)
{
	switch (field) {
	case FIELD_LABEL:
		snprintf(buf, len, "%s", cfg->label[0] ? cfg->label : "-");
		break;
	case FIELD_TYPE:
		snprintf(buf, len, "%s", pad_type_name(cfg->type));
		break;
	case FIELD_SAT:
		snprintf(buf, len, "%c", sat_name(cfg->sat));
		break;
	case FIELD_IGN:
		snprintf(buf, len, "CH%d", cfg->ign + 1);
		break;
	case FIELD_FILL:
		snprintf(buf, len, "CH%d", cfg->fill + 1);
		break;
	case FIELD_VENT:
		snprintf(buf, len, "CH%d", cfg->vent + 1);
		break;
	}
}

static void refresh_setup_view(const struct ctrl_view *v)
{
	static const char *const help[] = {
		[SETUP_FIELDS] = "Turn: field   Push: change   Hold: save   SAFE: cancel",
		[SETUP_VALUE] = "Turn: change value   Push: done   SAFE: undo",
		[SETUP_LABEL] = "Turn: character   Push: next   Hold: done   SAFE: undo",
	};
	const struct pad_setup *su = &v->setup;
	const struct pad_cfg *cfg = &su->cfg;
	bool editing_label = su->mode == SETUP_LABEL;
	char buf[24];

	lv_label_set_text_fmt(setup_title, "PAD %d SETUP", su->pad + 1);

	for (int f = 0; f < FIELD_COUNT; f++) {
		bool visible = f == FIELD_LABEL || f == FIELD_TYPE ||
			       ((f == FIELD_SAT || f == FIELD_IGN) && cfg->type != PAD_OFF) ||
			       cfg->type == PAD_HYBRID;

		if (!visible) {
			lv_obj_add_flag(setup_row[f], LV_OBJ_FLAG_HIDDEN);
			continue;
		}
		lv_obj_remove_flag(setup_row[f], LV_OBJ_FLAG_HIDDEN);
		set_inverted(setup_row[f], f == su->field && !editing_label);

		setup_field_text(cfg, f, buf, sizeof(buf));
		if (f == su->field && su->mode == SETUP_VALUE) {
			/* Arrows show that turning now changes the value */
			lv_label_set_text_fmt(setup_value[f], LV_SYMBOL_LEFT "  %s  " LV_SYMBOL_RIGHT,
					      buf);
		} else {
			lv_label_set_text(setup_value[f], buf);
		}
	}

	if (editing_label) {
		lv_obj_add_flag(setup_value[FIELD_LABEL], LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_remove_flag(setup_value[FIELD_LABEL], LV_OBJ_FLAG_HIDDEN);
	}
	for (int i = 0; i < PAD_LABEL_MAX; i++) {
		char c[2] = {su->label[i], '\0'};

		if (!editing_label) {
			lv_obj_add_flag(setup_char_box[i], LV_OBJ_FLAG_HIDDEN);
			continue;
		}
		lv_obj_remove_flag(setup_char_box[i], LV_OBJ_FLAG_HIDDEN);
		lv_label_set_text(setup_char[i], c);
		set_inverted(setup_char_box[i], i == su->pos);
	}

	lv_obj_set_pos(setup_cursor, 3, SETUP_Y0 + su->field * SETUP_H + 4);
	lv_label_set_text(setup_help, help[su->mode]);
	if (su->error) {
		lv_label_set_text_fmt(setup_error, LV_SYMBOL_WARNING " %s", su->error);
	} else {
		lv_label_set_text(setup_error, "");
	}
}

void ui_refresh(void)
{
	const struct ctrl_view *v = ctrl_view();
	int64_t now = k_uptime_get();

	if (v->state == CTRL_ARMED || v->state == CTRL_FIRING) {
		refresh_armed_view(v, now);
		show_only(armed_view);
	} else if (v->fault_active) {
		lv_label_set_text(fault_text, v->message);
		show_only(fault_view);
	} else if (v->setup.active) {
		refresh_setup_view(v);
		show_only(setup_view);
	} else if (v->settings.active) {
		refresh_settings_view(v);
		show_only(settings_view);
	} else {
		refresh_list_view(v, now);
		show_only(list_view);
	}
}
