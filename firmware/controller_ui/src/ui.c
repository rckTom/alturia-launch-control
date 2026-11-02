/*
 * 400x240 1-bit status display (Sharp Memory LCD class). There is no
 * touch: all input comes from the physical controls. Only black and
 * white exist, so emphasis is done with inversion, never with color.
 *
 * Every screen is a full-screen view, built once at start-up. Each one is
 * described and drawn in its own file under ui/:
 *
 *   list.c     SAFE and ARMING: the pads, grouped by satellite
 *   armed.c    ARMED: continuity table, valves, fire hold
 *   launch.c   FIRING: a rocket leaving the pad
 *   menu.c     the menu, the diagnostics menu and the messages
 *   pads.c     channel list and pad setup
 *   sats.c     satellites and pairing
 *   diag.c     button test and battery report
 *   dialogs.c  fault, power off and the splash
 *
 * widgets.c holds the shared styles and the pieces the views are made of.
 * ui_refresh() runs every controller tick: it refreshes the one view the
 * controller's state calls for and hides all others.
 */

#include <zephyr/kernel.h>
#include <lvgl.h>

#include "controller.h"
#include "inputs.h"
#include "ui.h"
#include "ui/views.h"
#include "ui/widgets.h"

/* How long an event stays up: progress briefly, refusals a bit longer */
#define INFO_SHOW_MS 4000
#define WARN_SHOW_MS 6000
#define SPLASH_MS    1500

static int64_t splash_until;

bool ui_message_recent(const struct ctrl_view *v, int64_t now)
{
	return v->message[0] &&
	       now - v->message_time < (v->message_level == MSG_INFO ? INFO_SHOW_MS : WARN_SHOW_MS);
}

void ui_set_message(lv_obj_t *label, const struct ctrl_view *v)
{
	ui_set_text_fmt(label, "%s%s", v->message_level == MSG_INFO ? "" : LV_SYMBOL_WARNING " ",
			v->message);
}

void ui_init(void)
{
	lv_obj_t *scr = lv_screen_active();

	lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

	ui_styles_init();
	ui_list_create(scr);
	ui_armed_create(scr);
	ui_launch_create(scr);
	ui_menus_create(scr);
	ui_pads_create(scr);
	ui_sats_create(scr);
	ui_diag_create(scr);
	ui_dialogs_create(scr);

	splash_until = k_uptime_get() + SPLASH_MS;
	ui_refresh();
}

/* The view the state calls for, refreshed. The first that applies wins. */
static lv_obj_t *current_view(const struct ctrl_view *v, int64_t now)
{
	if (v->state == CTRL_FIRING) {
		return ui_launch_refresh(v, now);
	}
	ui_launch_reset();

	if (v->powering_off) {
		return ui_splash_refresh(inputs_pressed(BTN_POWER)
					 ? "Switching off - release the power button"
					 : "Switching off...");
	}
	if (now < splash_until) {
		return ui_splash_refresh("Starting...");
	}
	if (v->power_confirm) {
		return ui_power_refresh();
	}
	if (v->state == CTRL_ARMED) {
		return ui_armed_refresh(v, now);
	}
	if (v->fault_active) {
		return ui_fault_refresh(v);
	}
	if (v->pair.active) {
		return ui_pair_refresh(v);
	}
	if (v->sats.active) {
		return ui_sats_refresh(v, now);
	}
	if (v->diag != DIAG_NONE) {
		return ui_diag_refresh(v);
	}
	if (v->setup.active) {
		return ui_setup_refresh(v);
	}
	if (v->pad_list.active) {
		return ui_padlist_refresh(v);
	}
	if (v->messages.active) {
		return ui_messages_refresh(v, now);
	}
	if (v->diag_menu.active) {
		return ui_diag_menu_refresh(v);
	}
	if (v->settings.active) {
		return ui_settings_refresh(v);
	}
	return ui_list_refresh(v, now);
}

void ui_refresh(void)
{
	ui_show_only(current_view(ctrl_view(), k_uptime_get()));
}
