/*
 * Building blocks of the views: shared styles, constructors for the
 * recurring elements, and setters that only touch LVGL when a value
 * changes (see widgets.c).
 */

#ifndef UI_WIDGETS_H_
#define UI_WIDGETS_H_

#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>

#include "fonts/ui_fonts.h"

#define SCREEN_W 400
#define SCREEN_H 240

/* Layout every view follows */
#define MARGIN       8    /* left edge of titles and lines of text */
#define TITLE_RULE_Y 27   /* the rule under the title bar */
#define ROW_X        18   /* list rows start here... */
#define CURSOR_X     3    /* ...and the cursor sits left of them */

/* Scrolling lists: rows under the title, a rule and a line of help below */
#define LIST_Y0      31
#define LIST_ROW_H   22
#define LIST_ROWS    8
#define LIST_H       (LIST_ROWS * LIST_ROW_H)
#define LIST_RULE_Y  (LIST_Y0 + LIST_H + 4)
#define LIST_ROW_W   386   /* leaves room for the scroll bar */

/*
 * Every box is opaque white with black text; ui_set_inverted() makes it
 * black with white text. These are added on top where a box needs more.
 */
extern lv_style_t ui_style_inverted;     /* added at LV_STATE_CHECKED */
extern lv_style_t ui_style_transparent;  /* a box that only groups others */
extern lv_style_t ui_style_round;        /* a circle */
extern lv_style_t ui_style_frame_thin;   /* 1 px black border */
extern lv_style_t ui_style_frame;        /* 2 px */
extern lv_style_t ui_style_frame_heavy;  /* 8 px, around dialogs */

void ui_styles_init(void);

/* Constructors */
lv_obj_t *ui_box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h);
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font);
lv_obj_t *ui_text_line(lv_obj_t *parent, const lv_font_t *font, int32_t w,
		       lv_text_align_t align);
void ui_rule(lv_obj_t *parent, int32_t y);
lv_obj_t *ui_title(lv_obj_t *view, const char *text);
lv_obj_t *ui_footer(lv_obj_t *view, int32_t rule_y, const char *text);
lv_obj_t *ui_cursor(lv_obj_t *view);

/*
 * Lists longer than the screen. ui_scroll() returns the first row to show,
 * moved from @p first only as far as needed to show rows @p top (a header
 * that belongs to the cursor, or the cursor itself) to @p cursor.
 * The scroll bar is a thin bar at the right edge, as long as the share of
 * the list on screen; hidden while everything fits.
 */
int ui_scroll(int first, int top, int cursor, int count, int rows);
lv_obj_t *ui_scrollbar(lv_obj_t *view, int32_t y, int32_t h);
void ui_scrollbar_set(lv_obj_t *bar, int32_t y, int32_t h, int first, int count, int rows);

/* A full-screen view. ui_show_only() shows one and hides all others. */
lv_obj_t *ui_view(lv_obj_t *scr, bool inverted);
void ui_show_only(lv_obj_t *view);

/* Setters that leave the panel alone when nothing changes */
void ui_set_text(lv_obj_t *label, const char *text);
void ui_set_text_fmt(lv_obj_t *label, const char *fmt, ...);
void ui_set_visible(lv_obj_t *obj, bool visible);
void ui_set_inverted(lv_obj_t *obj, bool inverted);

#endif /* UI_WIDGETS_H_ */
