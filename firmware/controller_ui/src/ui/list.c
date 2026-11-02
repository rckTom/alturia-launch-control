/*
 * Main list (SAFE, ARMING): the pads of every paired satellite, grouped
 * under a header row per satellite, and what ARM would do.
 *
 *  +------------------------------------------------+
 *  | SAFE              [CH 85%] [SATS 1/2]  /[## ]78% |  status strip
 *  |------------------------------------------------|
 *  | A  12.4V                               ready  ||  satellite header
 *  |# #  ALFA     RKT  CH1                   OK ####||  cursor: inverted row
 *  |     Pad 3    RKT  CH2                #OPEN#    |
 *  | B  12.1V                       #NO LINK 12s#   |
 *  | #   Pad 6    RKT  CH1                   --     |  # in front: ticked
 *  |------------------------------------------------|
 *  | ARM: arm ALFA   OK: tick       hold OK: menu   |  hint, or a message
 *  +------------------------------------------------+
 *
 * The cursor is the inverted row; a small square in front of a pad marks
 * it ticked. Pads stay where they are when their satellite's link goes:
 * they show "--" and arming them is refused with the reason in the hint.
 * When the list is longer than the screen, it scrolls with the cursor and
 * a bar on the right shows where it is.
 *
 * A message the operator has to see (a refused button, a launch, an arm
 * timeout) takes the hint line for a few seconds. Everything the screen
 * shows anyway, like a satellite's link going, is only logged; the menu's
 * Messages screen lists both.
 *
 * Before arming the satellites only check their outputs for shorts; the
 * continuity column shows "?" unless CONFIG_APP_PREARM_CONTINUITY says
 * their readings are real while disarmed.
 */

#include <string.h>

#include <zephyr/kernel.h>

#include "battery.h"
#include "views.h"
#include "widgets.h"

#define ROW_W       LIST_ROW_W
#define HINT_RULE_Y LIST_RULE_Y

/* Satellite battery: 3S pack, flag below 3.5 V per cell */
#define SAT_LOW_MV 10500

/* Controller battery in the status strip */
#define BATT_X     330
#define BATT_W     24
#define BATT_H     14
#define BATT_NUB_W 3
#define BATT_LOW   15   /* %, the percentage is inverted at or below */

/* What a row of the list shows */
enum entry_kind {
	ENTRY_SAT,     /* idx: satellite */
	ENTRY_PAD,     /* idx: pad */
	ENTRY_NO_PADS, /* idx: satellite */
};

struct entry {
	uint8_t kind;
	uint8_t idx;
};

#define MAX_ENTRIES (2 * NUM_SATS + NUM_PADS)

/*
 * One screen row. It shows a satellite header, a pad or a note with the
 * same objects, moved and hidden as needed (set_kind()), which keeps the
 * object count, and so LVGL's memory, down.
 */
struct row {
	lv_obj_t *box;
	lv_obj_t *mark;
	lv_obj_t *name;   /* satellite letter, pad name */
	lv_obj_t *info;   /* satellite battery, pad type, the note */
	lv_obj_t *chan;
	lv_obj_t *flag_box;
	lv_obj_t *flag;   /* satellite state, pad continuity */
	int kind;
};

static lv_obj_t *view;
static lv_obj_t *state_label;
static lv_obj_t *crowd_box;
static lv_obj_t *crowd_text;
static lv_obj_t *sats_box;
static lv_obj_t *sats_text;
static struct row rows[LIST_ROWS];
static lv_obj_t *scroll_thumb;
static lv_obj_t *empty;
static lv_obj_t *hint;
static lv_obj_t *hint_menu;

static lv_obj_t *batt_icon;   /* the bar and the terminal nub */
static lv_obj_t *batt_bar;
static lv_obj_t *batt_bolt;
static lv_obj_t *batt_text;

/* First entry on screen, kept across refreshes so that the list only
 * scrolls when the cursor would leave it.
 */
static int first_entry;

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
	lv_obj_set_pos(batt_text, BATT_X + BATT_W + 7, 6);
}

/* A small framed box in the status strip with a centered label */
static lv_obj_t *create_badge(int32_t x, int32_t w, lv_obj_t **text)
{
	lv_obj_t *box = ui_box(view, x, 3, w, 21);

	lv_obj_add_style(box, &ui_style_frame_thin, 0);
	*text = ui_label(box, FONT_SMALL);
	lv_obj_center(*text);
	return box;
}

static void create_status(void)
{
	state_label = ui_title(view, "");
	crowd_box = create_badge(148, 76, &crowd_text);
	sats_box = create_badge(230, 80, &sats_text);
	create_battery();
}

/* Ticked: a small square, black, or white on the inverted cursor row */
static void create_mark(struct row *r)
{
	r->mark = ui_box(r->box, 7, (LIST_ROW_H - 1 - 8) / 2, 8, 8);
}

static void create_row(int i)
{
	struct row *r = &rows[i];

	r->box = ui_box(view, 6, LIST_Y0 + i * LIST_ROW_H, ROW_W, LIST_ROW_H - 1);
	/* Satellite headers are underlined */
	lv_obj_set_style_border_side(r->box, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_color(r->box, lv_color_black(), 0);
	lv_obj_set_style_border_width(r->box, 0, 0);
	create_mark(r);

	r->name = ui_label(r->box, FONT_MEDIUM);
	lv_obj_set_width(r->name, 96);
	lv_label_set_long_mode(r->name, LV_LABEL_LONG_MODE_CLIP);

	r->info = ui_label(r->box, FONT_SMALL);
	r->chan = ui_label(r->box, FONT_SMALL);
	lv_obj_align(r->chan, LV_ALIGN_LEFT_MID, 172, 0);

	/* As wide as its text, at the right end */
	r->flag_box = ui_box(r->box, 0, 0, LV_SIZE_CONTENT, LIST_ROW_H - 4);
	lv_obj_set_style_pad_hor(r->flag_box, 4, 0);
	lv_obj_align(r->flag_box, LV_ALIGN_RIGHT_MID, -2, 0);
	r->flag = ui_label(r->flag_box, FONT_SMALL);
	lv_obj_align(r->flag, LV_ALIGN_LEFT_MID, 0, 0);

	r->kind = -1;
}

void ui_list_create(lv_obj_t *scr)
{
	view = ui_view(scr, false);
	create_status();

	for (int i = 0; i < LIST_ROWS; i++) {
		create_row(i);
	}
	scroll_thumb = ui_scrollbar(view, LIST_Y0, LIST_H);

	empty = ui_text_line(view, FONT_MEDIUM, SCREEN_W - 2 * MARGIN, LV_TEXT_ALIGN_CENTER);
	lv_obj_set_pos(empty, MARGIN, LIST_Y0 + LIST_H / 2 - 12);

	ui_rule(view, HINT_RULE_Y);
	hint = ui_text_line(view, FONT_SMALL, 268, LV_TEXT_ALIGN_LEFT);
	lv_obj_set_pos(hint, MARGIN, HINT_RULE_Y + 6);
	hint_menu = ui_text_line(view, FONT_SMALL, 116, LV_TEXT_ALIGN_RIGHT);
	lv_obj_set_pos(hint_menu, SCREEN_W - MARGIN - 116, HINT_RULE_Y + 6);
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

static void refresh_status(const struct ctrl_view *v, int paired, int connected)
{
	ui_set_text(state_label, v->state == CTRL_SAFE && paired && !connected
				 ? "NO LINK" : ctrl_state_name(v->state));

	ui_set_visible(crowd_box, v->channel_crowded);
	ui_set_text_fmt(crowd_text, "CH %u%%", v->channel_util);

	ui_set_visible(sats_box, paired > 0);
	ui_set_inverted(sats_box, connected < paired);
	ui_set_text_fmt(sats_text, "SATS %d/%d", connected, paired);

	refresh_battery();
}

/* The rows of the list, top to bottom. *cursor_entry: the cursor's, or -1. */
static int build_entries(const struct ctrl_view *v, struct entry *e, int *cursor_entry)
{
	int order[NUM_PADS];
	int n = ctrl_list_order(order);
	int k = 0;

	*cursor_entry = -1;
	for (int sat = 0; sat < NUM_SATS; sat++) {
		bool any = false;

		if (!v->sat[sat].uid) {
			continue;
		}
		e[k++] = (struct entry){ENTRY_SAT, sat};
		for (int i = 0; i < n; i++) {
			if (pads[order[i]].sat != sat) {
				continue;
			}
			if (order[i] == v->cursor) {
				*cursor_entry = k;
			}
			e[k++] = (struct entry){ENTRY_PAD, order[i]};
			any = true;
		}
		if (!any) {
			e[k++] = (struct entry){ENTRY_NO_PADS, sat};
		}
	}
	return k;
}

/* Scroll to the cursor, with its satellite's header on the first pad of one */
static void scroll_to(const struct entry *e, int count, int cursor_entry)
{
	int top = cursor_entry;

	if (top > 0 && e[top - 1].kind == ENTRY_SAT) {
		top--;
	}
	first_entry = ui_scroll(first_entry, top, cursor_entry, count, LIST_ROWS);
}

/* Arrange a row for what it shows next; -1 hides it */
static void set_kind(struct row *r, int kind)
{
	if (kind == r->kind) {
		return;
	}
	r->kind = kind;
	ui_set_visible(r->box, kind >= 0);
	lv_obj_set_style_border_width(r->box, kind == ENTRY_SAT ? 1 : 0, 0);
	ui_set_visible(r->mark, kind == ENTRY_PAD);
	ui_set_visible(r->name, kind != ENTRY_NO_PADS);
	ui_set_visible(r->chan, kind == ENTRY_PAD);
	ui_set_visible(r->flag_box, kind != ENTRY_NO_PADS);
	lv_obj_align(r->name, LV_ALIGN_LEFT_MID, kind == ENTRY_SAT ? 2 : 22, 0);
	lv_obj_align(r->info, LV_ALIGN_LEFT_MID, kind == ENTRY_PAD ? 128 : 26, 0);
	if (kind != ENTRY_PAD) {
		ui_set_inverted(r->box, false);
		ui_set_text(r->chan, "");
	}
	if (kind == ENTRY_NO_PADS) {
		ui_set_text(r->info, "no pads  -  hold OK: menu");
	}
}

/* Header: battery, and the satellite's state, inverted when it is a problem */
static void refresh_sat_row(const struct row *r, const struct sat_view *s, int sat,
			    int64_t now)
{
	bool low = s->have_status && s->st.vbat_mv < SAT_LOW_MV;
	bool alarm = true;

	ui_set_text_fmt(r->name, "%c", sat_name(sat));
	if (s->have_status) {
		ui_set_text_fmt(r->info, "%d.%dV", s->st.vbat_mv / 1000,
				(s->st.vbat_mv % 1000) / 100);
	} else {
		ui_set_text(r->info, "");
	}

	if (!s->link_ok) {
		int32_t age = s->rx_time >= 0 ? (int32_t)((now - s->rx_time) / 1000) : -1;

		if (age >= 0 && age < 100) {
			ui_set_text_fmt(r->flag, "NO LINK %ds", age);
		} else {
			ui_set_text(r->flag, "NO LINK");
		}
	} else if (low) {
		ui_set_text(r->flag, "BATTERY LOW");
	} else {
		alarm = false;
		if (!s->st.key_armed) {
			ui_set_text(r->flag, "pad key SAFE");
		} else {
			ui_set_text(r->flag, s->visible ? "ready" : "connecting");
		}
	}
	ui_set_inverted(r->flag_box, alarm);
}

/* The continuity column; *problem when it is one */
static const char *cont_text(const struct ctrl_view *v, int p, bool *problem)
{
	const struct sat_view *s = &v->sat[pads[p].sat];
	const char *why = v->pad[p].problem;

	*problem = false;
	if (!s->link_ok) {
		return "--";
	}
	if (!s->have_status ||
	    (v->state == CTRL_SAFE && !IS_ENABLED(CONFIG_APP_PREARM_CONTINUITY))) {
		return "?";
	}
	if (!why) {
		return "OK";
	}
	*problem = true;
	/* A rocket pad only has its igniter: "IGN OPEN" says no more than "OPEN" */
	return pads[p].type == PAD_HYBRID ? why : why + strlen("IGN ");
}

static void refresh_pad_row(const struct row *r, const struct ctrl_view *v, int p)
{
	const struct pad_cfg *cfg = &pads[p];
	bool focus = p == v->cursor;
	bool problem;
	const char *cont = cont_text(v, p, &problem);

	ui_set_inverted(r->box, focus);
	ui_set_inverted(r->mark, !focus);
	ui_set_visible(r->mark, v->selected & BIT64(p));

	ui_set_text(r->name, pad_name(p));
	ui_set_text(r->info, cfg->type == PAD_HYBRID ? "HYB" : "RKT");
	if (cfg->type == PAD_HYBRID) {
		ui_set_text_fmt(r->chan, "CH%d+%d+%d", cfg->ign + 1, cfg->fill + 1, cfg->vent + 1);
	} else {
		ui_set_text_fmt(r->chan, "CH%d", cfg->ign + 1);
	}

	ui_set_text(r->flag, cont);
	/* Problems stand out from their row, inverted or not */
	ui_set_inverted(r->flag_box, problem != focus);
}

static void refresh_rows(const struct ctrl_view *v, const struct entry *e, int count,
			 int64_t now)
{
	for (int i = 0; i < LIST_ROWS; i++) {
		struct row *r = &rows[i];
		int k = first_entry + i;

		set_kind(r, k < count ? e[k].kind : -1);
		if (r->kind == ENTRY_SAT) {
			refresh_sat_row(r, &v->sat[e[k].idx], e[k].idx, now);
		} else if (r->kind == ENTRY_PAD) {
			refresh_pad_row(r, v, e[k].idx);
		}
	}
	ui_scrollbar_set(scroll_thumb, LIST_Y0, LIST_H, first_entry, count, LIST_ROWS);
}

/* What the buttons do now, or why ARM would not; or the latest message */
static void refresh_hint(const struct ctrl_view *v, int64_t now, int paired, int listed)
{
	bool msg = ui_message_recent(v, now);
	char list[32];

	/* A message gets the whole line */
	lv_obj_set_width(hint, msg ? SCREEN_W - 2 * MARGIN : 268);
	ui_set_visible(hint_menu, !msg);
	if (msg) {
		ui_set_message(hint, v);
		return;
	}

	/* With pads ticked, how to clear them matters more than the menu */
	ui_set_text(hint_menu, v->state != CTRL_SAFE ? ""
			       : v->selected ? "BACK: clear" : "hold OK: menu");

	if (v->state == CTRL_ARMING) {
		pads_format(list, sizeof(list), v->selected);
		ui_set_text_fmt(hint, "Arming %s...   BACK: cancel", list);
	} else if (!paired) {
		ui_set_text(hint, "No satellites paired");
	} else if (!listed) {
		ui_set_text(hint, "No pads set up");
	} else if (v->arm_block) {
		ui_set_text_fmt(hint, LV_SYMBOL_WARNING " %s", v->arm_block);
	} else if (v->selected) {
		pads_format(list, sizeof(list), v->arm_set);
		ui_set_text_fmt(hint, "ARM: arm %s", list);
	} else {
		pads_format(list, sizeof(list), v->arm_set);
		ui_set_text_fmt(hint, "ARM: arm %s   OK: tick", list);
	}
}

lv_obj_t *ui_list_refresh(const struct ctrl_view *v, int64_t now)
{
	static struct entry entries[MAX_ENTRIES];
	int paired = 0;
	int connected = 0;
	int listed = 0;
	int cursor_entry;
	int count;

	for (int sat = 0; sat < NUM_SATS; sat++) {
		paired += v->sat[sat].uid != 0;
		connected += v->sat[sat].uid != 0 && v->sat[sat].visible;
	}
	for (int p = 0; p < NUM_PADS; p++) {
		listed += ctrl_pad_listed(p);
	}

	refresh_status(v, paired, connected);

	count = build_entries(v, entries, &cursor_entry);
	scroll_to(entries, count, cursor_entry);
	refresh_rows(v, entries, count, now);

	ui_set_visible(empty, !paired);
	ui_set_text(empty, "Hold OK and pair a satellite");

	refresh_hint(v, now, paired, listed);
	return view;
}
