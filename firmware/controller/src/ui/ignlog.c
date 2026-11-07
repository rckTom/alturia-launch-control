/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * Ignition log, from the diagnostics menu (struct ignlog_view):
 *  - Satellites: one row per slot, with how many records its index holds.
 *  - Records: the satellite's last channel pulses, newest first: channel
 *    and pad, peak current, when the igniter opened, how long ago.
 *  - Chart: one record's current over time, the whole pulse or its first
 *    IGNLOG_ZOOM_MS, with peak, opening time and the lowest fire rail
 *    voltage under it.
 * Records are downloaded when asked for (ignlog.h); the page says how far
 * the download has got.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "ignlog.h"
#include "views.h"
#include "widgets.h"

BUILD_ASSERT(NUM_SATS <= LIST_ROWS && IGNLOG_LIST_MAX <= LIST_ROWS);

/* Chart area, and one point per 2 px */
#define CHART_X      52
#define CHART_Y      34
#define CHART_W      336
#define CHART_H      136
#define CHART_POINTS (CHART_W / 2)
#define AXIS_Y       (CHART_Y + CHART_H + 2)
#define INFO_Y       (AXIS_Y + 20)
#define HELP_RULE_Y  (SCREEN_H - 26)

static lv_obj_t *view;
static lv_obj_t *title;
static lv_obj_t *hint;
static lv_obj_t *footer;

static lv_obj_t *row[LIST_ROWS];
#define COLS 5
static lv_obj_t *col[LIST_ROWS][COLS];
static lv_obj_t *empty;

static lv_obj_t *chart_page;
static lv_obj_t *chart;
static lv_chart_series_t *series;
static int32_t points[CHART_POINTS];
static lv_obj_t *y_top, *y_mid, *y_zero, *x_mid, *x_end;
static lv_obj_t *info;
static lv_obj_t *chart_msg;

/* What the chart shows now, to redraw only on a change */
static struct {
	int sat;
	uint32_t seq;
	bool zoom;
	const uint8_t *rec;
} shown = { .sat = -1 };

static void create_rows(void)
{
	/* #seq, channel and pad, peak, opening, age (right-aligned) */
	static const int32_t col_x[COLS] = { 6, 50, 158, 216, 0 };

	for (int r = 0; r < LIST_ROWS; r++) {
		row[r] = ui_box(view, 6, LIST_Y0 + r * LIST_ROW_H, LIST_ROW_W, LIST_ROW_H - 1);
		for (int c = 0; c < COLS; c++) {
			col[r][c] = ui_label(row[r], c == 0 ? FONT_MEDIUM : FONT_SMALL);
			lv_obj_align(col[r][c], LV_ALIGN_LEFT_MID, col_x[c], 0);
		}
		lv_obj_align(col[r][COLS - 1], LV_ALIGN_RIGHT_MID, -6, 0);
	}
	empty = ui_text_line(view, FONT_MEDIUM, SCREEN_W - 2 * MARGIN, LV_TEXT_ALIGN_CENTER);
	lv_obj_set_pos(empty, MARGIN, LIST_Y0 + LIST_H / 2 - 12);
}

static lv_obj_t *axis_label(lv_obj_t *parent, int32_t x, int32_t y, lv_text_align_t align)
{
	lv_obj_t *l = ui_text_line(parent, FONT_SMALL, 80, align);

	lv_obj_set_pos(l, x, y);
	return l;
}

static void create_chart(void)
{
	chart_page = ui_box(view, 0, TITLE_RULE_Y + 2, SCREEN_W, HELP_RULE_Y - TITLE_RULE_Y - 3);
	lv_obj_add_style(chart_page, &ui_style_transparent, 0);

	chart = lv_chart_create(chart_page);
	lv_obj_set_pos(chart, CHART_X, CHART_Y - TITLE_RULE_Y - 2);
	lv_obj_set_size(chart, CHART_W, CHART_H);
	lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
	lv_chart_set_div_line_count(chart, 5, 5);
	lv_obj_set_style_bg_color(chart, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(chart, lv_color_black(), 0);
	lv_obj_set_style_border_width(chart, 1, 0);
	lv_obj_set_style_radius(chart, 0, 0);
	lv_obj_set_style_pad_all(chart, 0, 0);
	/* Dotted grid, a 2 px trace without point markers */
	lv_obj_set_style_line_color(chart, lv_color_black(), LV_PART_MAIN);
	lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
	lv_obj_set_style_line_dash_width(chart, 1, LV_PART_MAIN);
	lv_obj_set_style_line_dash_gap(chart, 3, LV_PART_MAIN);
	lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
	lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
	lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
	series = lv_chart_add_series(chart, lv_color_black(), LV_CHART_AXIS_PRIMARY_Y);
	lv_chart_set_series_ext_y_array(chart, series, points);

	/* Axis labels: amps at the left, milliseconds underneath */
	y_top = axis_label(chart_page, CHART_X - 84, CHART_Y - TITLE_RULE_Y - 4, LV_TEXT_ALIGN_RIGHT);
	y_mid = axis_label(chart_page, CHART_X - 84, CHART_Y - TITLE_RULE_Y + CHART_H / 2 - 11,
			   LV_TEXT_ALIGN_RIGHT);
	y_zero = axis_label(chart_page, CHART_X - 84, CHART_Y - TITLE_RULE_Y + CHART_H - 18,
			    LV_TEXT_ALIGN_RIGHT);
	ui_set_text(y_zero, "0");
	x_mid = axis_label(chart_page, CHART_X + CHART_W / 2 - 40, AXIS_Y - TITLE_RULE_Y - 2,
			   LV_TEXT_ALIGN_CENTER);
	x_end = axis_label(chart_page, CHART_X + CHART_W - 80, AXIS_Y - TITLE_RULE_Y - 2,
			   LV_TEXT_ALIGN_RIGHT);

	info = ui_text_line(chart_page, FONT_SMALL, SCREEN_W - 2 * MARGIN, LV_TEXT_ALIGN_LEFT);
	lv_obj_set_pos(info, MARGIN, INFO_Y - TITLE_RULE_Y - 2);

	chart_msg = ui_text_line(chart_page, FONT_MEDIUM, SCREEN_W - 2 * MARGIN,
				 LV_TEXT_ALIGN_CENTER);
	lv_obj_set_pos(chart_msg, MARGIN, CHART_H / 2 - 4);
}

void ui_ignlog_create(lv_obj_t *scr)
{
	view = ui_view(scr, false);
	title = ui_title(view, "");
	hint = ui_label(view, FONT_SMALL);
	lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -8, 7);

	create_rows();
	create_chart();

	ui_rule(view, HELP_RULE_Y);
	footer = ui_text_line(view, FONT_SMALL, SCREEN_W - 2 * MARGIN, LV_TEXT_ALIGN_LEFT);
	lv_obj_set_pos(footer, MARGIN, HELP_RULE_Y + 5);
}

/* "1.76 A" */
static void amps_text(char *buf, size_t len, int32_t ma)
{
	snprintf(buf, len, "%d.%02d A", ma / 1000, (ma % 1000) / 10);
}

/* Axis labels: "2.5 A" */
static void amps_short(char *buf, size_t len, int32_t ma)
{
	snprintf(buf, len, "%d.%d A", ma / 1000, (ma % 1000) / 100);
}

static void age_text(char *buf, size_t len, const struct ignlog_entry *e,
		     const struct ignlog_store_view *iv)
{
	uint32_t s;

	/* The satellite's uptime started over: no telling how long ago */
	if (e->boot != iv->boots) {
		snprintf(buf, len, "reboot");
		return;
	}
	s = (iv->now_ms - e->t_fire_ms) / 1000;
	if (s < 120) {
		snprintf(buf, len, "%u s", s);
	} else if (s < 7200) {
		snprintf(buf, len, "%u min", s / 60);
	} else {
		snprintf(buf, len, "%u h", s / 3600);
	}
}

static void open_text(char *buf, size_t len, const struct ignlog_entry *e)
{
	if (e->flags & IGNLOG_F_NO_CURRENT) {
		snprintf(buf, len, "no current");
	} else if (e->open_idx == IGNLOG_OPEN_NONE) {
		snprintf(buf, len, "never opened");
	} else {
		snprintf(buf, len, "open %u ms", (uint32_t)e->open_idx * e->period_us / 1000);
	}
}

/* "CH3 ALFA", or "CH3" for a channel no pad uses */
static void channel_text(char *buf, size_t len, int sat, int ch)
{
	int p = ctrl_channel_pad(sat, ch);

	if (p >= 0) {
		snprintf(buf, len, "CH%d %s", ch + 1, pad_name(p));
	} else {
		snprintf(buf, len, "CH%d", ch + 1);
	}
}

/* What the download is doing, for the hint and the empty pages */
static const char *progress_text(char *buf, size_t len, int sat, uint32_t seq)
{
	struct ignlog_progress p;

	ignlog_progress(&p);
	if (p.sat != sat || (seq && p.seq != seq)) {
		return NULL;
	}
	if (p.state == IGNLOG_FAILED) {
		snprintf(buf, len, "%s", p.error);
	} else if (p.state != IGNLOG_BUSY) {
		return NULL;
	} else if (p.paused) {
		snprintf(buf, len, "Waiting for the link...");
	} else if (p.seq && p.total) {
		snprintf(buf, len, "Downloading %u / %u bytes", (unsigned)p.done,
			 (unsigned)p.total);
	} else {
		snprintf(buf, len, "Asking satellite %c...", sat_name(sat));
	}
	return buf;
}

static void show_rows(int n)
{
	for (int r = 0; r < LIST_ROWS; r++) {
		ui_set_visible(row[r], r < n);
	}
}

static void refresh_sats(const struct ctrl_view *v)
{
	struct ignlog_store_view iv;

	ui_set_text(title, "DIAGNOSTICS  -  IGNITION LOG");
	ui_set_text(hint, "");
	ui_set_visible(chart_page, false);
	ui_set_visible(empty, false);
	show_rows(NUM_SATS);

	for (int sat = 0; sat < NUM_SATS; sat++) {
		const struct sat_view *s = &v->sat[sat];

		ui_set_inverted(row[sat], sat == v->ignlog.sat);
		ui_set_text_fmt(col[sat][0], "%c", sat_name(sat));
		ui_set_text(col[sat][1], !s->uid ? "not paired" :
					 s->joined ? "connected" : "not connected");
		ui_set_text(col[sat][2], "");
		ui_set_text(col[sat][3], "");
		if (ignlog_index(sat, &iv)) {
			ui_set_text_fmt(col[sat][4], "%u record%s", iv.count,
					iv.count == 1 ? "" : "s");
		} else {
			ui_set_text(col[sat][4], "");
		}
	}
	ui_set_text(footer, LV_SYMBOL_UP LV_SYMBOL_DOWN " satellite    OK: records    BACK: back");
}

static void refresh_list(const struct ctrl_view *v)
{
	const struct ignlog_view *il = &v->ignlog;
	struct ignlog_store_view iv;
	char buf[48];
	const char *busy = progress_text(buf, sizeof(buf), il->sat, 0);
	bool have = ignlog_index(il->sat, &iv);
	int n = have ? iv.count : 0;

	ui_set_text_fmt(title, "IGNITION LOG  -  SATELLITE %c", sat_name(il->sat));
	ui_set_visible(chart_page, false);
	ui_set_text(hint, busy && n ? "updating" : "");
	show_rows(n);

	for (int r = 0; r < n; r++) {
		const struct ignlog_entry *e = &iv.e[iv.count - 1 - r];
		char peak[16], open[24], age[24], ch[24];

		ui_set_inverted(row[r], r == il->cursor);
		channel_text(ch, sizeof(ch), il->sat, e->channel);
		amps_text(peak, sizeof(peak), ignlog_ma(e->peak_code, iv.scale_na));
		open_text(open, sizeof(open), e);
		age_text(age, sizeof(age), e, &iv);
		ui_set_text_fmt(col[r][0], "#%u", e->seq);
		ui_set_text(col[r][1], ch);
		ui_set_text(col[r][2], peak);
		ui_set_text(col[r][3], e->flags & IGNLOG_F_FAULT ? "FAULT" : open);
		ui_set_text(col[r][4], age);
	}

	ui_set_visible(empty, n == 0);
	if (n == 0) {
		ui_set_text(empty, busy ? busy : have ? "No ignitions recorded" :
				   "No answer yet");
	}
	ui_set_text(footer, LV_SYMBOL_UP LV_SYMBOL_DOWN
		    " record   OK: chart   Hold OK: reload   BACK");
}

/* A round full scale above the peak: 0.5 A steps, 2 A steps above 5 A */
static int32_t full_scale_ma(int32_t peak_ma)
{
	int32_t step = peak_ma > 5000 ? 2000 : 500;

	return MAX(step, (peak_ma / step + 1) * step);
}

static void draw_chart(const uint8_t *rec, bool zoom)
{
	struct ignlog_meta m;
	uint32_t n_win;
	uint32_t window_us;
	uint32_t npts;
	int32_t fs;
	char a[16];

	if (ignlog_meta_get(rec, IGNLOG_HDR_LEN, &m) || m.e.period_us == 0) {
		return;
	}

	/*
	 * The window: the zoom, or the whole record rounded up to a round
	 * number of milliseconds. Its samples, one point per bucket of
	 * them, at the bucket's peak; none past the record's end.
	 */
	if (zoom) {
		window_us = IGNLOG_ZOOM_MS * 1000;
	} else {
		uint32_t round_us = m.e.n_samples * m.e.period_us > 200000 ? 100000 : 10000;

		window_us = ROUND_UP((uint32_t)m.e.n_samples * m.e.period_us, round_us);
	}
	n_win = DIV_ROUND_UP(window_us, m.e.period_us);
	npts = MAX(MIN(n_win, CHART_POINTS), 2);
	for (uint32_t p = 0; p < npts; p++) {
		uint32_t from = p * n_win / npts;
		uint32_t to = MAX((p + 1) * n_win / npts, from + 1);
		uint16_t peak = 0;

		if (from >= m.e.n_samples) {
			points[p] = LV_CHART_POINT_NONE;
			continue;
		}
		for (uint32_t i = from; i < to && i < m.e.n_samples; i++) {
			peak = MAX(peak, ignlog_sample(rec, i));
		}
		points[p] = ignlog_ma(peak, m.scale_na);
	}

	fs = full_scale_ma(ignlog_ma(m.e.peak_code, m.scale_na));
	lv_chart_set_point_count(chart, npts);
	lv_chart_set_series_ext_y_array(chart, series, points);
	lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, fs);
	lv_chart_refresh(chart);

	amps_short(a, sizeof(a), fs);
	ui_set_text(y_top, a);
	amps_short(a, sizeof(a), fs / 2);
	ui_set_text(y_mid, a);
	ui_set_text_fmt(x_mid, "%u", window_us / 2000);
	ui_set_text_fmt(x_end, "%u ms", window_us / 1000);

	{
		char peak[16], open[24];

		amps_text(peak, sizeof(peak), ignlog_ma(m.e.peak_code, m.scale_na));
		open_text(open, sizeof(open), &m.e);
		ui_set_text_fmt(info, "Peak %s   %s   V_PYRO min %u.%u V%s%s", peak, open,
				m.vpyro_min_mv / 1000, (m.vpyro_min_mv % 1000) / 100,
				m.e.flags & IGNLOG_F_FAULT ? "   FAULT" : "",
				m.e.flags & IGNLOG_F_TRUNCATED ? "   cut short" : "");
	}
}

static void refresh_chart(const struct ctrl_view *v)
{
	const struct ignlog_view *il = &v->ignlog;
	struct ignlog_store_view iv;
	struct ignlog_entry e = { .channel = 0 };
	size_t len;
	const uint8_t *rec = ignlog_record(il->sat, il->seq, &len);
	char buf[48];
	char ch[24];

	if (ignlog_index(il->sat, &iv)) {
		for (int i = 0; i < iv.count; i++) {
			if (iv.e[i].seq == il->seq) {
				e = iv.e[i];
			}
		}
	}
	channel_text(ch, sizeof(ch), il->sat, e.channel);
	ui_set_text_fmt(title, "SATELLITE %c  #%u  %s", sat_name(il->sat), il->seq, ch);
	ui_set_text_fmt(hint, il->zoom ? "first %d ms" : "whole pulse", IGNLOG_ZOOM_MS);
	show_rows(0);
	ui_set_visible(empty, false);
	ui_set_visible(chart_page, true);

	ui_set_visible(chart, rec != NULL);
	ui_set_visible(y_top, rec != NULL);
	ui_set_visible(y_mid, rec != NULL);
	ui_set_visible(y_zero, rec != NULL);
	ui_set_visible(x_mid, rec != NULL);
	ui_set_visible(x_end, rec != NULL);
	ui_set_visible(info, rec != NULL);
	ui_set_visible(chart_msg, rec == NULL);

	if (!rec) {
		const char *busy = progress_text(buf, sizeof(buf), il->sat, il->seq);

		ui_set_text(chart_msg, busy ? busy : "Not downloaded");
		shown.rec = NULL;
	} else if (rec != shown.rec || il->sat != shown.sat || il->seq != shown.seq ||
		   il->zoom != shown.zoom) {
		draw_chart(rec, il->zoom);
		shown.rec = rec;
		shown.sat = il->sat;
		shown.seq = il->seq;
		shown.zoom = il->zoom;
	}
	ui_set_text(footer, LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " zoom   " LV_SYMBOL_UP LV_SYMBOL_DOWN
		    " older / newer   BACK: back");
}

lv_obj_t *ui_ignlog_refresh(const struct ctrl_view *v)
{
	switch (v->ignlog.page) {
	case IGNLOG_PAGE_SATS:
		refresh_sats(v);
		break;
	case IGNLOG_PAGE_LIST:
		refresh_list(v);
		break;
	case IGNLOG_PAGE_CHART:
		refresh_chart(v);
		break;
	}
	return view;
}
