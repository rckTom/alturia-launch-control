/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * Stacker (game.c), from the diagnostics menu: the well in the middle,
 * as tall as the screen, the score on the left and the next shape on the
 * right.
 *
 * The blocks are not LVGL objects: the well and the preview each draw
 * their cells as plain rectangles when LVGL redraws them, which needs
 * no memory from the LVGL pool. They are invalidated only when the game
 * changes.
 */

#include "game.h"
#include "views.h"
#include "widgets.h"

#define CELL     12   /* pitch: an 11 px block and a 1 px gap */
#define FRAME    2
/* Walls left and right only: the screen's edges are the top and the floor */
#define WELL_W   (GAME_COLS * CELL + 2 * FRAME)
#define WELL_X   ((SCREEN_W - WELL_W) / 2)
#define PANEL_W  (WELL_X - 2 * MARGIN)
#define RIGHT_X  (WELL_X + WELL_W + MARGIN)
#define NEXT_W   (4 * CELL + 1 + 2 * FRAME)

BUILD_ASSERT(GAME_ROWS * CELL == SCREEN_H, "the well fills the screen's height");

enum stat { STAT_SCORE, STAT_ROWS, STAT_LEVEL, STAT_BEST, STAT_COUNT };

static lv_obj_t *view;
static lv_obj_t *well;
static lv_obj_t *next_box;
static lv_obj_t *stat_value[STAT_COUNT];
static lv_obj_t *over_box;
static const struct game_view *shown;
static uint32_t drawn_version;

static void draw_cell(lv_layer_t *layer, const lv_area_t *inner, int col, int row, bool solid)
{
	lv_draw_rect_dsc_t dsc;
	lv_area_t a = {
		.x1 = inner->x1 + 1 + col * CELL,
		.y1 = inner->y1 + 1 + row * CELL,
	};

	/* The gap is left and above, so the bottom row ends on the edge */
	a.x2 = a.x1 + CELL - 2;
	a.y2 = a.y1 + CELL - 2;

	lv_draw_rect_dsc_init(&dsc);
	if (solid) {
		dsc.bg_color = lv_color_black();
		dsc.bg_opa = LV_OPA_COVER;
	} else {
		/* The landing place: an outline */
		dsc.bg_opa = LV_OPA_TRANSP;
		dsc.border_color = lv_color_black();
		dsc.border_opa = LV_OPA_COVER;
		dsc.border_width = 1;
	}
	lv_draw_rect(layer, &dsc, &a);
}

/* Draws rows of cells, a bit per column, inside the object's frame */
static void draw_rows(lv_event_t *e, const uint16_t *solid, const uint16_t *outline, int rows,
		      int cols, int32_t frame_y)
{
	lv_obj_t *obj = lv_event_get_target_obj(e);
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t inner;

	lv_obj_get_coords(obj, &inner);
	lv_area_increase(&inner, -FRAME, -frame_y);

	for (int r = 0; r < rows; r++) {
		for (int c = 0; c < cols; c++) {
			if (solid[r] & BIT(c)) {
				draw_cell(layer, &inner, c, r, true);
			} else if (outline && (outline[r] & BIT(c))) {
				draw_cell(layer, &inner, c, r, false);
			}
		}
	}
}

static void draw_well(lv_event_t *e)
{
	uint16_t solid[GAME_ROWS];

	if (!shown) {
		return;
	}
	for (int r = 0; r < GAME_ROWS; r++) {
		solid[r] = shown->well[r] | shown->falling[r];
	}
	draw_rows(e, solid, shown->ghost, GAME_ROWS, GAME_COLS, 0);
}

static void draw_next(lv_event_t *e)
{
	if (shown) {
		draw_rows(e, shown->next, NULL, 4, 4, FRAME);
	}
}

static lv_obj_t *cells_box(int32_t x, int32_t y, int32_t w, int32_t h, lv_event_cb_t cb)
{
	lv_obj_t *box = ui_box(view, x, y, w, h);

	lv_obj_add_style(box, &ui_style_frame, 0);
	lv_obj_add_event_cb(box, cb, LV_EVENT_DRAW_MAIN_END, NULL);
	return box;
}

void ui_game_create(lv_obj_t *scr)
{
	static const char *const stat_names[STAT_COUNT] = {
		[STAT_SCORE] = "Score",
		[STAT_ROWS] = "Rows",
		[STAT_LEVEL] = "Level",
		[STAT_BEST] = "Best",
	};
	lv_obj_t *label;

	view = ui_view(scr, false);

	label = ui_label(view, FONT_MEDIUM);
	ui_set_text(label, "STACKER");
	lv_obj_set_pos(label, MARGIN, 5);
	ui_set_inverted(ui_box(view, MARGIN, TITLE_RULE_Y, PANEL_W, 1), true);

	/* The score large, the rest a line each */
	label = ui_label(view, FONT_SMALL);
	ui_set_text(label, stat_names[STAT_SCORE]);
	lv_obj_set_pos(label, MARGIN, 40);
	stat_value[STAT_SCORE] = ui_text_line(view, FONT_LARGE, PANEL_W, LV_TEXT_ALIGN_RIGHT);
	lv_obj_set_pos(stat_value[STAT_SCORE], MARGIN, 60);

	for (int i = STAT_ROWS; i < STAT_COUNT; i++) {
		int32_t y = 110 + (i - STAT_ROWS) * 32;

		label = ui_label(view, FONT_SMALL);
		ui_set_text(label, stat_names[i]);
		lv_obj_set_pos(label, MARGIN, y + 2);

		stat_value[i] = ui_text_line(view, FONT_MEDIUM, PANEL_W - 50, LV_TEXT_ALIGN_RIGHT);
		lv_obj_set_pos(stat_value[i], MARGIN + 50, y);
	}

	well = cells_box(WELL_X, 0, WELL_W, SCREEN_H, draw_well);
	lv_obj_set_style_border_side(well, LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT, 0);

	label = ui_label(view, FONT_SMALL);
	ui_set_text(label, "Next");
	lv_obj_set_pos(label, RIGHT_X, 6);
	next_box = cells_box(RIGHT_X, 30, NEXT_W, NEXT_W, draw_next);

	/* Over the middle of the well when the game has ended */
	over_box = ui_box(view, WELL_X - 6, SCREEN_H / 2 - 34, WELL_W + 12, 68);
	ui_set_inverted(over_box, true);
	label = ui_label(over_box, FONT_MEDIUM);
	ui_set_text(label, "GAME OVER");
	lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 10);
	label = ui_label(over_box, FONT_SMALL);
	ui_set_text(label, "OK: play again");
	lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -10);
}

lv_obj_t *ui_game_refresh(int64_t now)
{
	const struct game_view *g = game_tick(now);

	shown = g;
	if (g->version != drawn_version) {
		drawn_version = g->version;
		lv_obj_invalidate(well);
		lv_obj_invalidate(next_box);
	}
	ui_set_text_fmt(stat_value[STAT_SCORE], "%u", g->score);
	ui_set_text_fmt(stat_value[STAT_ROWS], "%u", g->lines);
	ui_set_text_fmt(stat_value[STAT_LEVEL], "%u", g->level + 1);
	ui_set_text_fmt(stat_value[STAT_BEST], "%u", g->best);
	ui_set_visible(over_box, g->phase == GAME_OVER);
	return view;
}
