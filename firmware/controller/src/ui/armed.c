/*
 * Armed view (ARMED): whole screen inverted, with the arm timeout, the
 * armed pads' continuity (problems inverted again) and the fire hold
 * progress. Up to three rocket pads, the usual case, are shown large:
 *
 *  ARMED    [! CONTINUITY: 1 PAD]           0:42
 *
 *   ALFA                 A  CH1               OK
 *   Pad 2                A  CH2           [OPEN]
 *
 *          Hold FIRE to launch  -  BACK disarms
 *        [##########                          ]
 *
 * More pads, or any hybrid, get a table with the valve state:
 *
 *  PAD      IGN    FILL   VENT   VALVES
 *  ALFA     OK     OK     OK     FILL 0:12
 *  Pad 2    OPEN
 *
 * Rows are sorted: continuity problems first, then hybrid pads (for their
 * valve state), then the rest. Up and down scroll when they do not fit.
 *
 * Continuity here is what the satellite reads from the armed rail.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "views.h"
#include "widgets.h"

enum { COL_PAD, COL_IGN, COL_FILL, COL_VENT, COL_VALVES, COL_COUNT };

#define TABLE_Y0 34
#define TABLE_H  22

/* Large rows, for a few rocket pads */
#define BIG_ROWS 3
#define BIG_Y0   40
#define BIG_H    44

static const int32_t col_x[COL_COUNT] = {6, 96, 152, 208, 268};
static const int32_t col_w[COL_COUNT] = {86, 52, 52, 56, 124};

static lv_obj_t *view;
static lv_obj_t *warn_box;
static lv_obj_t *warn;
static lv_obj_t *timer;
static lv_obj_t *table;
static lv_obj_t *cell_box[ARMED_TABLE_ROWS][COL_COUNT];
static lv_obj_t *cell_text[ARMED_TABLE_ROWS][COL_COUNT];
static lv_obj_t *table_summary;
static lv_obj_t *big_row[BIG_ROWS];
static lv_obj_t *big_name[BIG_ROWS];
static lv_obj_t *big_chan[BIG_ROWS];
static lv_obj_t *big_cont_box[BIG_ROWS];
static lv_obj_t *big_cont[BIG_ROWS];
static lv_obj_t *hint;
static lv_obj_t *fire_bar;

static void create_table(void)
{
	static const char *const heads[COL_COUNT] = {"PAD", "IGN", "FILL", "VENT", "VALVES"};
	table = ui_box(view, 0, TABLE_Y0, SCREEN_W, (ARMED_TABLE_ROWS + 2) * TABLE_H);

	ui_set_inverted(table, true);
	for (int c = 0; c < COL_COUNT; c++) {
		lv_obj_t *head = ui_label(table, FONT_SMALL);

		ui_set_text(head, heads[c]);
		lv_obj_set_pos(head, col_x[c] + 3, 2);
	}
	for (int r = 0; r < ARMED_TABLE_ROWS; r++) {
		for (int c = 0; c < COL_COUNT; c++) {
			cell_box[r][c] = ui_box(table, col_x[c], (r + 1) * TABLE_H, col_w[c],
						TABLE_H - 2);
			cell_text[r][c] = ui_label(cell_box[r][c], FONT_SMALL);
			lv_obj_align(cell_text[r][c], LV_ALIGN_LEFT_MID, 3, 0);
		}
	}
	/* Scroll indicator below the rows */
	table_summary = ui_label(table, FONT_SMALL);
	lv_obj_set_pos(table_summary, col_x[COL_PAD] + 3, (ARMED_TABLE_ROWS + 1) * TABLE_H + 2);
}

static void create_big_rows(void)
{
	for (int r = 0; r < BIG_ROWS; r++) {
		big_row[r] = ui_box(view, 0, BIG_Y0 + r * BIG_H, SCREEN_W, BIG_H - 4);
		ui_set_inverted(big_row[r], true);

		big_name[r] = ui_label(big_row[r], FONT_LARGE);
		lv_obj_set_width(big_name[r], 150);
		lv_label_set_long_mode(big_name[r], LV_LABEL_LONG_MODE_CLIP);
		lv_obj_align(big_name[r], LV_ALIGN_LEFT_MID, 12, 0);

		big_chan[r] = ui_label(big_row[r], FONT_SMALL);
		lv_obj_align(big_chan[r], LV_ALIGN_LEFT_MID, 170, 0);

		/* As wide as its text, at the right end */
		big_cont_box[r] = ui_box(big_row[r], 0, 0, LV_SIZE_CONTENT, BIG_H - 8);
		lv_obj_set_style_pad_hor(big_cont_box[r], 6, 0);
		lv_obj_align(big_cont_box[r], LV_ALIGN_RIGHT_MID, -8, 0);
		big_cont[r] = ui_label(big_cont_box[r], FONT_LARGE);
		lv_obj_align(big_cont[r], LV_ALIGN_LEFT_MID, 0, 0);
	}
}

/* White on black, like the view */
static void create_fire_bar(void)
{
	fire_bar = lv_bar_create(view);
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

void ui_armed_create(lv_obj_t *scr)
{
	lv_obj_t *title;

	view = ui_view(scr, true);

	title = ui_label(view, FONT_MEDIUM);
	ui_set_text(title, "ARMED");
	lv_obj_set_pos(title, MARGIN, 6);

	timer = ui_label(view, FONT_LARGE);
	lv_obj_align(timer, LV_ALIGN_TOP_RIGHT, -8, 2);

	/* Continuity warning in the title bar, visible whatever is scrolled */
	warn_box = ui_box(view, 96, 4, 200, 24);
	warn = ui_label(warn_box, FONT_SMALL);
	lv_obj_center(warn);

	create_table();
	create_big_rows();

	hint = ui_text_line(view, FONT_SMALL, SCREEN_W - 2 * MARGIN, LV_TEXT_ALIGN_CENTER);
	lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);

	create_fire_bar();
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
	ui_set_text(cell_text[row][col], text);
	/* The view is inverted; problems flip back to black on white */
	ui_set_inverted(cell_box[row][col], !problem);
}

static void set_cont_cell(int row, int col, const struct sat_view *s, int ch)
{
	set_cell(row, col, cont_text(s, ch), s->st.cont[ch] != CONT_OK);
}

static void refresh_table_row(const struct ctrl_view *v, int row, int p, int64_t now)
{
	const struct pad_cfg *cfg = &pads[p];
	const struct sat_view *s = &v->sat[cfg->sat];
	uint8_t valves = s->st.valve_mask;
	char buf[24];

	set_cell(row, COL_PAD, pad_name(p), false);
	set_cont_cell(row, COL_IGN, s, cfg->ign);

	if (cfg->type != PAD_HYBRID) {
		set_cell(row, COL_FILL, "", false);
		set_cell(row, COL_VENT, "", false);
		set_cell(row, COL_VALVES, "", false);
		return;
	}

	set_cont_cell(row, COL_FILL, s, cfg->fill);
	set_cont_cell(row, COL_VENT, s, cfg->vent);

	/* Valve state as reported by the satellite, not as commanded */
	if (valves & BIT(cfg->fill)) {
		int32_t t = (int32_t)((now - v->pad[p].fill_start) / 1000);

		snprintf(buf, sizeof(buf), "FILL %d:%02d", t / 60, t % 60);
	} else {
		snprintf(buf, sizeof(buf), "%s", valves & BIT(cfg->vent) ? "CLOSED" : "VENTING");
	}
	set_cell(row, COL_VALVES, buf, false);
}

/* The armed pads in table order: continuity problems, then hybrids, then the rest */
static int sort_pads(const struct ctrl_view *v, int order[NUM_PADS])
{
	int n = 0;

	for (int rank = 0; rank < 3; rank++) {
		for (int p = 0; p < NUM_PADS; p++) {
			int r = v->pad[p].problem ? 0 : pads[p].type == PAD_HYBRID ? 1 : 2;

			if ((v->selected & BIT64(p)) && r == rank) {
				order[n++] = p;
			}
		}
	}
	return n;
}

static void refresh_table(const struct ctrl_view *v, const int *order, int n, int64_t now)
{
	int first = MIN(v->table_scroll, MAX(n - ARMED_TABLE_ROWS, 0));
	int below = MAX(n - first - ARMED_TABLE_ROWS, 0);

	for (int row = 0; row < ARMED_TABLE_ROWS; row++) {
		bool used = first + row < n;

		for (int c = 0; c < COL_COUNT; c++) {
			ui_set_visible(cell_box[row][c], used);
		}
		if (used) {
			refresh_table_row(v, row, order[first + row], now);
		}
	}

	if (first || below) {
		ui_set_text_fmt(table_summary, "%s %d above      %s %d below",
				LV_SYMBOL_UP, first, LV_SYMBOL_DOWN, below);
	} else {
		ui_set_text(table_summary, "");
	}
}

/* Rocket pads only: the igniter's continuity, large */
static void refresh_big(const struct ctrl_view *v, const int *order, int n)
{
	for (int r = 0; r < BIG_ROWS; r++) {
		const struct pad_cfg *cfg;
		const struct sat_view *s;

		if (r >= n) {
			continue;
		}
		cfg = &pads[order[r]];
		s = &v->sat[cfg->sat];
		ui_set_text(big_name[r], pad_name(order[r]));
		ui_set_text_fmt(big_chan[r], "%c  CH%d", sat_name(cfg->sat), cfg->ign + 1);
		ui_set_text(big_cont[r], cont_text(s, cfg->ign));
		ui_set_inverted(big_cont_box[r], s->st.cont[cfg->ign] == CONT_OK);
	}
}

lv_obj_t *ui_armed_refresh(const struct ctrl_view *v, int64_t now)
{
	int32_t s = (v->arm_remaining_ms + 999) / 1000;
	int order[NUM_PADS];
	int n = sort_pads(v, order);
	bool hybrid = false;
	int problems = 0;
	bool big;

	ui_set_text_fmt(timer, "%d:%02d", s / 60, s % 60);

	for (int i = 0; i < n; i++) {
		problems += v->pad[order[i]].problem != NULL;
		hybrid |= pads[order[i]].type == PAD_HYBRID;
	}
	ui_set_visible(warn_box, problems);
	if (problems) {
		ui_set_text_fmt(warn, LV_SYMBOL_WARNING " CONTINUITY: %d PAD%s", problems,
				problems == 1 ? "" : "S");
	}

	/* The large rows when they say everything, the table otherwise */
	big = n <= BIG_ROWS && !hybrid;

	ui_set_visible(table, !big);
	if (big) {
		refresh_big(v, order, n);
	} else {
		refresh_table(v, order, n, now);
	}
	for (int r = 0; r < BIG_ROWS; r++) {
		ui_set_visible(big_row[r], big && r < n);
	}

	if (ui_message_recent(v, now)) {
		/* A refused button, or a valve change that went wrong */
		ui_set_message(hint, v);
	} else if (v->launch_block) {
		ui_set_text_fmt(hint, LV_SYMBOL_WARNING " %s  -  BACK disarms", v->launch_block);
	} else if (v->arm_timer_paused) {
		ui_set_text(hint, "Arm timer paused while filling  -  BACK disarms");
	} else {
		ui_set_text(hint, hybrid ? "FILL  /  VENT  /  hold FIRE  -  BACK disarms"
					 : "Hold FIRE to launch  -  BACK disarms");
	}
	lv_bar_set_value(fire_bar, v->fire_hold_ms, LV_ANIM_OFF);
	return view;
}
