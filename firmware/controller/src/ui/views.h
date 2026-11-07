/*
 * The views ui_refresh() switches between. Each is built once by its
 * _create() and updated from the controller's state by its _refresh(),
 * which returns the view for ui_show_only().
 */

#ifndef UI_VIEWS_H_
#define UI_VIEWS_H_

#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>

#include "controller.h"

/* The latest event, for a few seconds after it happened */
bool ui_message_recent(const struct ctrl_view *v, int64_t now);
void ui_set_message(lv_obj_t *label, const struct ctrl_view *v);

/* list.c */
void ui_list_create(lv_obj_t *scr);
lv_obj_t *ui_list_refresh(const struct ctrl_view *v, int64_t now);

/* armed.c */
void ui_armed_create(lv_obj_t *scr);
lv_obj_t *ui_armed_refresh(const struct ctrl_view *v, int64_t now);

/* launch.c; ui_launch_reset() makes the next launch start over */
void ui_launch_create(lv_obj_t *scr);
lv_obj_t *ui_launch_refresh(const struct ctrl_view *v, int64_t now);
void ui_launch_reset(void);

/* menu.c */
void ui_menus_create(lv_obj_t *scr);
lv_obj_t *ui_settings_refresh(const struct ctrl_view *v);
lv_obj_t *ui_diag_menu_refresh(const struct ctrl_view *v);
lv_obj_t *ui_messages_refresh(const struct ctrl_view *v, int64_t now);

/* pads.c */
void ui_pads_create(lv_obj_t *scr);
lv_obj_t *ui_padlist_refresh(const struct ctrl_view *v);
lv_obj_t *ui_setup_refresh(const struct ctrl_view *v);

/* sats.c */
void ui_sats_create(lv_obj_t *scr);
lv_obj_t *ui_sats_refresh(const struct ctrl_view *v, int64_t now);
lv_obj_t *ui_pair_refresh(const struct ctrl_view *v);

/* diag.c */
void ui_diag_create(lv_obj_t *scr);
lv_obj_t *ui_diag_refresh(const struct ctrl_view *v);

/* ignlog.c: the ignition log pages */
void ui_ignlog_create(lv_obj_t *scr);
lv_obj_t *ui_ignlog_refresh(const struct ctrl_view *v);

/* game.c: also runs the game, see game.h */
void ui_game_create(lv_obj_t *scr);
lv_obj_t *ui_game_refresh(int64_t now);

/* dialogs.c */
void ui_dialogs_create(lv_obj_t *scr);
lv_obj_t *ui_fault_refresh(const struct ctrl_view *v);
lv_obj_t *ui_power_refresh(void);
lv_obj_t *ui_splash_refresh(const char *text);

#endif /* UI_VIEWS_H_ */
