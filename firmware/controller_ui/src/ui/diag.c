/*
 * Diagnostics pages, opened from the diagnostics menu:
 *  - Buttons: a box per button, inverted while it is down, with its press
 *    count, and the last few presses and releases. Every button is shown
 *    instead of acting, so only holding BACK leaves.
 *  - Battery: everything the fuel gauge and the charger report.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "battery.h"
#include "inputs.h"
#include "views.h"
#include "widgets.h"

/* Button test */
#define BTN_COLS  4
#define BTN_Y0    33
#define BTN_W     94
#define BTN_H     36
#define BTN_LOG_Y 161

static lv_obj_t *view;
static lv_obj_t *title;
static lv_obj_t *hint;
static lv_obj_t *page[DIAG_COUNT];
static lv_obj_t *btn_box[BTN_COUNT];
static lv_obj_t *btn_text[BTN_COUNT];
static lv_obj_t *btn_log[INPUTS_LOG_LEN];

/* Battery page: two columns of name / value rows */
enum {
	BROW_SOC, BROW_VOLTAGE, BROW_TO_EMPTY, BROW_TO_FULL, BROW_PGOOD, BROW_INPUT, BROW_STATUS,
	BROW_HEALTH, BROW_ICHG, BROW_VREG, BROW_IINLIM, BROW_VINDPM, BROW_IPRECHG, BROW_ITERM,
	BROW_COUNT,
};

#define BROW_PER_COL 7
#define BROW_Y0      34
#define BROW_H       25
#define BVALUE_W     96

static lv_obj_t *batt_value[BROW_COUNT];

static void create_buttons_page(lv_obj_t *p)
{
	for (int b = 0; b < BTN_COUNT; b++) {
		btn_box[b] = ui_box(p, 6 + (b % BTN_COLS) * (BTN_W + 4),
				    BTN_Y0 + (b / BTN_COLS) * (BTN_H + 4), BTN_W, BTN_H);
		lv_obj_add_style(btn_box[b], &ui_style_frame_thin, 0);
		btn_text[b] = ui_label(btn_box[b], FONT_SMALL);
		lv_obj_center(btn_text[b]);
	}
	if (!IS_ENABLED(CONFIG_APP_KEY_SWITCH)) {
		ui_set_visible(btn_box[BTN_KEY], false);
	}

	ui_rule(p, BTN_LOG_Y - 4);

	for (int i = 0; i < INPUTS_LOG_LEN; i++) {
		btn_log[i] = ui_label(p, FONT_SMALL);
		lv_obj_set_pos(btn_log[i], MARGIN, BTN_LOG_Y + i * 19);
	}
}

static void create_battery_page(lv_obj_t *p)
{
	static const char *const names[BROW_COUNT] = {
		"Charge", "Voltage", "To empty", "To full", "Power good", "Input power",
		"Charger", "Health", "Fast charge", "Charge to", "Input limit",
		"Input min.", "Precharge", "Termination",
	};

	for (int r = 0; r < BROW_COUNT; r++) {
		int32_t x = r < BROW_PER_COL ? MARGIN : SCREEN_W / 2 + 6;
		int32_t y = BROW_Y0 + (r % BROW_PER_COL) * BROW_H;
		lv_obj_t *name = ui_label(p, FONT_SMALL);

		ui_set_text(name, names[r]);
		lv_obj_set_pos(name, x, y);

		/* Right-aligned at the column's end */
		batt_value[r] = ui_label(p, FONT_SMALL);
		lv_obj_set_width(batt_value[r], BVALUE_W);
		lv_obj_set_style_text_align(batt_value[r], LV_TEXT_ALIGN_RIGHT, 0);
		lv_obj_set_pos(batt_value[r], x + SCREEN_W / 2 - 14 - BVALUE_W, y);
	}
	ui_rule(p, BROW_Y0 + BROW_PER_COL * BROW_H + 2);
}

void ui_diag_create(lv_obj_t *scr)
{
	view = ui_view(scr, false);
	title = ui_title(view, "");

	hint = ui_label(view, FONT_SMALL);
	lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -8, 7);

	/* One page per diagnostics menu entry, over the title bar */
	for (int i = 0; i < DIAG_COUNT; i++) {
		page[i] = ui_box(view, 0, 0, SCREEN_W, SCREEN_H);
		lv_obj_add_style(page[i], &ui_style_transparent, 0);
	}
	create_buttons_page(page[DIAG_BUTTONS]);
	create_battery_page(page[DIAG_BATTERY]);
}

static void refresh_buttons_page(void)
{
	struct inputs_diag d;

	inputs_diag(&d);
	for (int b = 0; b < BTN_COUNT; b++) {
		ui_set_inverted(btn_box[b], d.pressed & BIT(b));
		ui_set_text_fmt(btn_text[b], "%s  %u", button_name(b), d.count[b]);
	}
	for (int i = 0; i < INPUTS_LOG_LEN; i++) {
		const struct button_event *ev = &d.log[i];
		int32_t ms = (int32_t)(ev->time % 1000);
		int32_t s = (int32_t)(ev->time / 1000);

		if (i >= d.n_log) {
			ui_set_text(btn_log[i], "");
		} else if (ev->pressed) {
			ui_set_text_fmt(btn_log[i], "%d.%03d s   %s pressed", s, ms,
					button_name(ev->button));
		} else {
			ui_set_text_fmt(btn_log[i], "%d.%03d s   %s released after %d ms", s, ms,
					button_name(ev->button), ev->held_ms);
		}
	}
}

static void minutes_text(char *buf, size_t len, uint32_t min)
{
	if (min == 0) {
		snprintf(buf, len, "-");
	} else {
		snprintf(buf, len, "%u h %02u min", min / 60, min % 60);
	}
}

static void refresh_battery_page(void)
{
	struct battery_info b;
	char buf[BROW_COUNT][24];

	battery_get(&b);
	for (int r = 0; r < BROW_COUNT; r++) {
		snprintf(buf[r], sizeof(buf[r]), "-");
	}
	if (b.gauge) {
		snprintf(buf[BROW_SOC], 24, "%u %%", b.soc_pct);
		snprintf(buf[BROW_VOLTAGE], 24, "%u.%03u V", b.voltage_mv / 1000, b.voltage_mv % 1000);
		minutes_text(buf[BROW_TO_EMPTY], 24, b.to_empty_min);
		minutes_text(buf[BROW_TO_FULL], 24, b.to_full_min);
	}
	if (b.pgood >= 0) {
		snprintf(buf[BROW_PGOOD], 24, "%s", b.pgood ? "yes" : "no");
	}
	if (b.charger) {
		snprintf(buf[BROW_INPUT], 24, "%s", b.online ? "yes" : "no");
		snprintf(buf[BROW_STATUS], 24, "%s", battery_status_text(b.status));
		snprintf(buf[BROW_HEALTH], 24, "%s", battery_health_text(b.health));
		snprintf(buf[BROW_ICHG], 24, "%u mA", b.ichg_ma);
		snprintf(buf[BROW_VREG], 24, "%u.%03u V", b.vreg_mv / 1000, b.vreg_mv % 1000);
		snprintf(buf[BROW_IINLIM], 24, "%u mA", b.iinlim_ma);
		snprintf(buf[BROW_VINDPM], 24, "%u.%02u V", b.vindpm_mv / 1000,
			 (b.vindpm_mv % 1000) / 10);
		snprintf(buf[BROW_IPRECHG], 24, "%u mA", b.iprechg_ma);
		snprintf(buf[BROW_ITERM], 24, "%u mA", b.iterm_ma);
	}
	for (int r = 0; r < BROW_COUNT; r++) {
		ui_set_text(batt_value[r], buf[r]);
	}
}

lv_obj_t *ui_diag_refresh(const struct ctrl_view *v)
{
	for (int i = 0; i < DIAG_COUNT; i++) {
		ui_set_visible(page[i], i == v->diag);
	}
	if (v->diag == DIAG_BATTERY) {
		ui_set_text(title, "BATTERY");
		ui_set_text(hint, "BACK: back");
		refresh_battery_page();
	} else {
		ui_set_text(title, "BUTTONS");
		ui_set_text(hint, "Hold BACK to leave");
		refresh_buttons_page();
	}
	return view;
}
