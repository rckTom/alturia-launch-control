/*
 * Building blocks of the views.
 *
 * Boxes do not use the theme: their whole look comes from the shared
 * styles here, so the views only place things. Inversion is a state: a
 * box in LV_STATE_CHECKED picks up ui_style_inverted, and the labels in it
 * inherit its text color. In every list the cursor is the inverted row.
 *
 * LVGL redraws a label or object whenever its text, a style or a state is
 * set, even to the value it already has. ui_refresh() runs every
 * controller tick, so the setters below only touch LVGL on a change, and
 * the panel is only written (and the CPU only kept awake) when something
 * on it changes.
 */

#include <stdarg.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "widgets.h"

lv_style_t ui_style_inverted;
lv_style_t ui_style_transparent;
lv_style_t ui_style_round;
lv_style_t ui_style_frame_thin;
lv_style_t ui_style_frame;
lv_style_t ui_style_frame_heavy;

static lv_style_t style_box;

static const lv_font_t *const fonts[] = {FONT_SMALL, FONT_MEDIUM, FONT_LARGE};
static lv_style_t style_font[ARRAY_SIZE(fonts)];

#define MAX_VIEWS 16

static lv_obj_t *views[MAX_VIEWS];
static size_t view_count;

static void init_frame(lv_style_t *style, int32_t width)
{
	lv_style_init(style);
	lv_style_set_border_width(style, width);
	lv_style_set_border_color(style, lv_color_black());
}

void ui_styles_init(void)
{
	lv_style_init(&style_box);
	lv_style_set_bg_opa(&style_box, LV_OPA_COVER);
	lv_style_set_bg_color(&style_box, lv_color_white());
	lv_style_set_text_color(&style_box, lv_color_black());

	/* Opaque as well, for the labels that invert themselves */
	lv_style_init(&ui_style_inverted);
	lv_style_set_bg_opa(&ui_style_inverted, LV_OPA_COVER);
	lv_style_set_bg_color(&ui_style_inverted, lv_color_black());
	lv_style_set_text_color(&ui_style_inverted, lv_color_white());

	lv_style_init(&ui_style_transparent);
	lv_style_set_bg_opa(&ui_style_transparent, LV_OPA_TRANSP);

	lv_style_init(&ui_style_round);
	lv_style_set_radius(&ui_style_round, LV_RADIUS_CIRCLE);

	init_frame(&ui_style_frame_thin, 1);
	init_frame(&ui_style_frame, 2);
	init_frame(&ui_style_frame_heavy, 8);

	ARRAY_FOR_EACH(fonts, i) {
		lv_style_init(&style_font[i]);
		lv_style_set_text_font(&style_font[i], fonts[i]);
	}
}

lv_obj_t *ui_box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
	lv_obj_t *box = lv_obj_create(parent);

	lv_obj_remove_style_all(box);
	lv_obj_add_style(box, &style_box, 0);
	lv_obj_add_style(box, &ui_style_inverted, LV_STATE_CHECKED);
	lv_obj_set_scrollable(box, false);
	lv_obj_set_pos(box, x, y);
	lv_obj_set_size(box, w, h);
	return box;
}

/* font is one of FONT_SMALL, FONT_MEDIUM and FONT_LARGE */
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font)
{
	lv_obj_t *label = lv_label_create(parent);

	ARRAY_FOR_EACH(fonts, i) {
		if (fonts[i] == font) {
			lv_obj_add_style(label, &style_font[i], 0);
		}
	}
	ui_set_text(label, "");
	return label;
}

/* One line of text in a fixed width, cut off with "..." when too long */
lv_obj_t *ui_text_line(lv_obj_t *parent, const lv_font_t *font, int32_t w,
		       lv_text_align_t align)
{
	lv_obj_t *label = ui_label(parent, font);

	lv_obj_set_width(label, w);
	lv_obj_set_style_text_align(label, align, 0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
	return label;
}

/* A thin black line across the view */
void ui_rule(lv_obj_t *parent, int32_t y)
{
	ui_set_inverted(ui_box(parent, 6, y, SCREEN_W - 12, 1), true);
}

/* Title at the top left, with the rule under it */
lv_obj_t *ui_title(lv_obj_t *view, const char *text)
{
	lv_obj_t *title = ui_label(view, FONT_MEDIUM);

	ui_set_text(title, text);
	lv_obj_set_pos(title, MARGIN, 5);
	ui_rule(view, TITLE_RULE_Y);
	return title;
}

/* A rule with a line of text under it, usually what the buttons do */
lv_obj_t *ui_footer(lv_obj_t *view, int32_t rule_y, const char *text)
{
	lv_obj_t *footer;

	ui_rule(view, rule_y);
	footer = ui_label(view, FONT_SMALL);
	ui_set_text(footer, text);
	lv_obj_set_pos(footer, MARGIN, rule_y + 8);
	return footer;
}

int ui_scroll(int first, int top, int cursor, int count, int rows)
{
	if (cursor >= 0) {
		if (top < first) {
			first = top;
		}
		if (cursor >= first + rows) {
			first = cursor - rows + 1;
		}
	}
	return CLAMP(first, 0, MAX(count - rows, 0));
}

lv_obj_t *ui_scrollbar(lv_obj_t *view, int32_t y, int32_t h)
{
	lv_obj_t *bar = ui_box(view, SCREEN_W - 5, y, 3, h);

	ui_set_inverted(bar, true);
	ui_set_visible(bar, false);
	return bar;
}

void ui_scrollbar_set(lv_obj_t *bar, int32_t y, int32_t h, int first, int count, int rows)
{
	int32_t len;

	ui_set_visible(bar, count > rows);
	if (count <= rows) {
		return;
	}
	len = MAX(h * rows / count, 12);
	lv_obj_set_pos(bar, SCREEN_W - 5, y + (h - len) * first / (count - rows));
	lv_obj_set_height(bar, len);
}

lv_obj_t *ui_view(lv_obj_t *scr, bool inverted)
{
	lv_obj_t *view = ui_box(scr, 0, 0, SCREEN_W, SCREEN_H);

	__ASSERT_NO_MSG(view_count < MAX_VIEWS);
	views[view_count++] = view;
	ui_set_inverted(view, inverted);
	return view;
}

void ui_show_only(lv_obj_t *view)
{
	for (size_t i = 0; i < view_count; i++) {
		ui_set_visible(views[i], views[i] == view);
	}
}

void ui_set_text(lv_obj_t *label, const char *text)
{
	if (strcmp(lv_label_get_text(label), text) != 0) {
		lv_label_set_text(label, text);
	}
}

void ui_set_text_fmt(lv_obj_t *label, const char *fmt, ...)
{
	char buf[160];
	va_list args;

	va_start(args, fmt);
	lv_vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	ui_set_text(label, buf);
}

/* LVGL itself ignores flags and states that are already set */
void ui_set_visible(lv_obj_t *obj, bool visible)
{
	lv_obj_set_hidden(obj, !visible);
}

void ui_set_inverted(lv_obj_t *obj, bool inverted)
{
	lv_obj_set_state(obj, LV_STATE_CHECKED, inverted);
}
