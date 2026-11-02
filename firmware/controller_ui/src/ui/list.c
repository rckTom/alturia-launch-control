/*
 * List view (SAFE, ARMING), with the controller's own battery after the
 * state (a bolt while charging):
 *  +------------------------------------------------+
 *  | SAFE  /[###  ]78%  [A  12.4V]  [#B##NO#LINK#]  |
 *  |------------------------------------------------|
 *  |   ALFA     HYB   A        (only pads of        |
 *  | > #Pad 2###RKT###A#######connected satellites)#| <- cursor, selected
 *  | ...                                            |
 *  |------------------------------------------------|
 *  | next step, or why arming is blocked            |
 *  +------------------------------------------------+
 */

#include <zephyr/kernel.h>

#include "battery.h"
#include "views.h"
#include "widgets.h"

struct row {
	lv_obj_t *box;
	lv_obj_t *name;
	lv_obj_t *type;
	lv_obj_t *sat;
};

static lv_obj_t *view;
static lv_obj_t *state_label;
static lv_obj_t *sat_box[NUM_SATS];
static lv_obj_t *sat_text[NUM_SATS];
static struct row rows[NUM_PADS];
static lv_obj_t *cursor;
static lv_obj_t *footer;

/* Controller battery in the title bar */
#define BATT_X     94
#define BATT_W     24
#define BATT_H     14
#define BATT_NUB_W 3
#define BATT_LOW   15   /* %, the percentage is inverted at or below */

static lv_obj_t *batt_icon;   /* the bar and the terminal nub */
static lv_obj_t *batt_bar;
static lv_obj_t *batt_bolt;
static lv_obj_t *batt_text;

static void create_battery(void)
{
	static lv_point_precise_t bolt_pts[] = {{6, 0}, {1, 8}, {6, 8}, {2, 16}};
	lv_obj_t *nub;

	batt_bolt = lv_line_create(view);
	lv_line_set_points(batt_bolt, bolt_pts, ARRAY_SIZE(bolt_pts));
	lv_obj_set_style_line_width(batt_bolt, 2, 0);
	lv_obj_set_style_line_color(batt_bolt, lv_color_black(), 0);
	lv_obj_set_pos(batt_bolt, BATT_X - 10, 5);

	batt_icon = ui_box(view, BATT_X, 7, BATT_W + BATT_NUB_W, BATT_H);
	lv_obj_add_style(batt_icon, &ui_style_transparent, 0);

	/* The charge fills it from the left, black up to the border: padding
	 * as wide as the frame keeps the fill inside it.
	 */
	batt_bar = lv_bar_create(batt_icon);
	lv_obj_remove_style_all(batt_bar);
	lv_obj_add_style(batt_bar, &ui_style_frame, 0);
	lv_obj_set_style_pad_all(batt_bar, 2, 0);
	lv_obj_add_style(batt_bar, &ui_style_inverted, LV_PART_INDICATOR);
	lv_obj_set_size(batt_bar, BATT_W, BATT_H);
	lv_bar_set_range(batt_bar, 0, 100);

	nub = ui_box(batt_icon, BATT_W, 4, BATT_NUB_W, BATT_H - 8);
	ui_set_inverted(nub, true);
	ui_set_visible(batt_icon, false);

	batt_text = ui_label(view, FONT_SMALL);
	lv_obj_add_style(batt_text, &ui_style_inverted, LV_STATE_CHECKED);
	lv_obj_set_pos(batt_text, BATT_X + BATT_W + 6, 6);
}

void ui_list_create(lv_obj_t *scr)
{
	int32_t bottom = PAD_ROW_Y0 + NUM_PADS * PAD_ROW_H;

	view = ui_view(scr, false);
	state_label = ui_title(view, "");
	create_battery();

	/* One box per satellite, inverted when it needs attention */
	for (int sat = 0; sat < NUM_SATS; sat++) {
		sat_box[sat] = ui_box(view, SCREEN_W - 6 - (NUM_SATS - sat) * 108, 3, 104, 21);
		sat_text[sat] = ui_label(sat_box[sat], FONT_SMALL);
		lv_obj_center(sat_text[sat]);
	}

	for (int p = 0; p < NUM_PADS; p++) {
		struct row *r = &rows[p];

		r->box = ui_box(view, ROW_X, PAD_ROW_Y0 + p * PAD_ROW_H, SCREEN_W - 22,
				PAD_ROW_H - 2);

		r->name = ui_label(r->box, FONT_MEDIUM);
		lv_obj_set_width(r->name, 88);
		lv_label_set_long_mode(r->name, LV_LABEL_LONG_MODE_CLIP);
		lv_obj_align(r->name, LV_ALIGN_LEFT_MID, 6, 0);

		r->type = ui_label(r->box, FONT_SMALL);
		lv_obj_align(r->type, LV_ALIGN_LEFT_MID, 100, 0);

		r->sat = ui_label(r->box, FONT_SMALL);
		lv_obj_align(r->sat, LV_ALIGN_LEFT_MID, 142, 0);
	}
	cursor = ui_cursor(view);

	ui_rule(view, bottom + 1);
	footer = ui_text_line(view, FONT_SMALL, SCREEN_W - 2 * MARGIN, LV_TEXT_ALIGN_LEFT);
	lv_obj_set_pos(footer, MARGIN, bottom + 9);
}

static void refresh_battery(void)
{
	struct battery_info b;
	bool charging;
	bool low;

	battery_get(&b);
	charging = b.gauge && battery_charging(&b);
	low = b.gauge && b.soc_pct <= BATT_LOW && !charging;

	ui_set_visible(batt_icon, b.gauge);
	ui_set_visible(batt_bolt, charging);
	/* Low: the percentage turns white on black */
	ui_set_inverted(batt_text, low);
	if (!b.gauge) {
		ui_set_text(batt_text, "");
		return;
	}
	lv_bar_set_value(batt_bar, b.soc_pct, LV_ANIM_OFF);
	ui_set_text_fmt(batt_text, low ? " %u%% " : "%u%%", b.soc_pct);
}

static void refresh_sat(int sat, const struct sat_view *s)
{
	/* 3S pack: flag below 3.5 V per cell */
	bool low = s->st.vbat_mv < 10500;

	if (!s->uid) {
		ui_set_text_fmt(sat_text[sat], "%c  NOT PAIRED", sat_name(sat));
		ui_set_inverted(sat_box[sat], false);
	} else if (s->visible) {
		ui_set_text_fmt(sat_text[sat], "%c  %d.%dV%s", sat_name(sat),
				s->st.vbat_mv / 1000, (s->st.vbat_mv % 1000) / 100,
				low ? " LOW" : "");
		ui_set_inverted(sat_box[sat], low);
	} else if (s->link_ok) {
		ui_set_text_fmt(sat_text[sat], "%c  connecting", sat_name(sat));
		ui_set_inverted(sat_box[sat], false);
	} else {
		ui_set_text_fmt(sat_text[sat], "%c  NO LINK", sat_name(sat));
		ui_set_inverted(sat_box[sat], true);
	}
}

static void refresh_row(const struct ctrl_view *v, int slot, int p)
{
	const struct row *r = &rows[slot];

	ui_set_visible(r->box, true);
	ui_set_inverted(r->box, v->selected & BIT(p));
	ui_set_text(r->name, pad_name(p));
	ui_set_text(r->type, pads[p].type == PAD_HYBRID ? "HYB" : "RKT");
	ui_set_text_fmt(r->sat, "%c", sat_name(pads[p].sat));
}

/* The most recent event, otherwise the next step */
static void refresh_footer(const struct ctrl_view *v, int64_t now, bool any_paired,
			   bool any_visible, bool cursor_on_pad)
{
	if (ui_message_recent(v, now)) {
		ui_set_message(footer, v);
	} else if (v->state == CTRL_ARMING) {
		ui_set_text(footer, "Arming...");
	} else if (!any_paired) {
		ui_set_text(footer, "No satellites paired. Hold OK for settings.");
	} else if (!any_visible) {
		ui_set_text(footer, "No satellites connected");
	} else if (!cursor_on_pad) {
		ui_set_text(footer, "No pads available. Hold OK for settings.");
	} else if (v->arm_block) {
		ui_set_text(footer, v->arm_block);
	} else if (v->channel_crowded && v->other_nets) {
		ui_set_text_fmt(footer, LV_SYMBOL_WARNING " Channel %u%% busy, %u other networks",
				v->channel_util, v->other_nets);
	} else if (v->channel_crowded) {
		ui_set_text_fmt(footer, LV_SYMBOL_WARNING " Channel %u%% busy", v->channel_util);
	} else {
		char list[PADS_FORMAT_LEN];

		pads_format(list, sizeof(list), v->selected);
		ui_set_text_fmt(footer, "Press ARM to arm %s", list);
	}
}

lv_obj_t *ui_list_refresh(const struct ctrl_view *v, int64_t now)
{
	bool any_visible = false;
	bool any_paired = false;
	bool cursor_on_pad = false;
	int slot = 0;

	for (int sat = 0; sat < NUM_SATS; sat++) {
		any_visible |= v->sat[sat].visible;
		any_paired |= v->sat[sat].uid != 0;
		refresh_sat(sat, &v->sat[sat]);
	}

	ui_set_text(state_label, any_visible || v->state != CTRL_SAFE
				 ? ctrl_state_name(v->state) : "NO LINK");
	refresh_battery();

	/* Only pads that can be used right now, packed from the top */
	for (int p = 0; p < NUM_PADS; p++) {
		if (!ctrl_pad_visible(p)) {
			continue;
		}
		if (p == v->cursor) {
			cursor_on_pad = true;
			lv_obj_set_pos(cursor, CURSOR_X, PAD_ROW_Y0 + slot * PAD_ROW_H + 3);
		}
		refresh_row(v, slot++, p);
	}
	for (; slot < NUM_PADS; slot++) {
		ui_set_visible(rows[slot].box, false);
	}
	ui_set_visible(cursor, cursor_on_pad);

	refresh_footer(v, now, any_paired, any_visible, cursor_on_pad);
	return view;
}
