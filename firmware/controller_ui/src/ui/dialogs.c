/*
 * Fault: one sentence in a heavy frame, until BACK acknowledges it.
 *
 * Power off: asks before dropping the power latch.
 *
 * Splash: the logo while starting up and while switching off.
 */

#include "views.h"
#include "widgets.h"

LV_IMAGE_DECLARE(ui_logo);

static lv_obj_t *fault_view;
static lv_obj_t *fault_text;
static lv_obj_t *power_view;
static lv_obj_t *splash_view;
static lv_obj_t *splash_text;

/* A heavy frame, a large line of text and a small one at the bottom */
static lv_obj_t *create_dialog(lv_obj_t *scr, const char *title, int32_t title_y,
			       const char *bottom)
{
	lv_obj_t *view = ui_view(scr, false);
	lv_obj_t *label;

	lv_obj_add_style(view, &ui_style_frame_heavy, 0);

	label = ui_label(view, FONT_LARGE);
	ui_set_text(label, title);
	lv_obj_align(label, LV_ALIGN_CENTER, 0, title_y);

	label = ui_label(view, FONT_SMALL);
	ui_set_text(label, bottom);
	lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -20);
	return view;
}

static void create_splash(lv_obj_t *scr)
{
	lv_obj_t *logo;

	splash_view = ui_view(scr, false);

	/* A1 image: drawn in the recolor, opaque where the logo is */
	logo = lv_image_create(splash_view);
	lv_image_set_src(logo, &ui_logo);
	lv_obj_set_style_image_recolor(logo, lv_color_black(), 0);
	lv_obj_set_style_image_recolor_opa(logo, LV_OPA_COVER, 0);
	lv_obj_align(logo, LV_ALIGN_CENTER, 0, -24);

	splash_text = ui_label(splash_view, FONT_SMALL);
	lv_obj_set_width(splash_text, SCREEN_W - 2 * MARGIN);
	lv_obj_set_style_text_align(splash_text, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(splash_text, LV_ALIGN_BOTTOM_MID, 0, -24);
}

void ui_dialogs_create(lv_obj_t *scr)
{
	fault_view = create_dialog(scr, LV_SYMBOL_WARNING "  FAULT", -40,
				   "Press BACK to acknowledge");
	fault_text = ui_label(fault_view, FONT_MEDIUM);
	lv_obj_set_width(fault_text, SCREEN_W - 60);
	lv_obj_set_style_text_align(fault_text, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(fault_text, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_align(fault_text, LV_ALIGN_CENTER, 0, 10);

	power_view = create_dialog(scr, "Switch off?", -20, "OK: switch off    BACK: cancel");

	create_splash(scr);
}

lv_obj_t *ui_fault_refresh(const struct ctrl_view *v)
{
	ui_set_text(fault_text, v->message);
	return fault_view;
}

lv_obj_t *ui_power_refresh(void)
{
	return power_view;
}

lv_obj_t *ui_splash_refresh(const char *text)
{
	ui_set_text(splash_text, text);
	return splash_view;
}
