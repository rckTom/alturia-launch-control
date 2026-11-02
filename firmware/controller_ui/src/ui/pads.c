/*
 * Pads: every channel of every satellite with pads or a pairing, and the
 * pad that uses it. OK on a channel sets its pad up, or a new rocket pad
 * if the channel is free. Left and right jump a satellite.
 *
 *  MENU  -  PADS
 *  A  51A7E11172000000                   paired
 *  CH1   ALFA    hybrid igniter
 *  CH2   Pad 2   rocket
 *  CH3   free
 *  ...
 *
 * Pad setup: label, type, satellite and channels of one pad. The focused
 * field is inverted; the label is edited in one box per character.
 */

#include <stdio.h>

#include "views.h"
#include "widgets.h"

/* Channel list */
#define ROWS_PER_SAT (1 + NUM_CHANNELS)

struct chan_row {
	lv_obj_t *head;
	lv_obj_t *head_name;
	lv_obj_t *head_state;
	lv_obj_t *chan;
	lv_obj_t *ch;
	lv_obj_t *name;
	lv_obj_t *role;
};

static lv_obj_t *padlist_view;
static struct chan_row padlist_rows[LIST_ROWS];
static lv_obj_t *padlist_bar;
static lv_obj_t *padlist_empty;
static int padlist_first;

/* Pad setup */
#define SETUP_Y0 33
#define SETUP_H  26

static const char *const field_names[FIELD_COUNT] = {
	"Label", "Type", "Satellite", "Igniter", "Fill valve", "Vent valve",
};

static lv_obj_t *setup_view;
static lv_obj_t *setup_title;
static lv_obj_t *setup_row[FIELD_COUNT];
static lv_obj_t *setup_value[FIELD_COUNT];
static lv_obj_t *setup_char_box[PAD_LABEL_MAX];
static lv_obj_t *setup_char[PAD_LABEL_MAX];
static lv_obj_t *setup_help;
static lv_obj_t *setup_error;

static void create_padlist(lv_obj_t *scr)
{
	padlist_view = ui_view(scr, false);
	ui_title(padlist_view, "MENU  -  PADS");

	for (int i = 0; i < LIST_ROWS; i++) {
		struct chan_row *r = &padlist_rows[i];
		int32_t y = LIST_Y0 + i * LIST_ROW_H;

		/* Satellite: underlined */
		r->head = ui_box(padlist_view, 6, y, LIST_ROW_W, LIST_ROW_H - 1);
		lv_obj_set_style_border_side(r->head, LV_BORDER_SIDE_BOTTOM, 0);
		lv_obj_set_style_border_width(r->head, 1, 0);
		lv_obj_set_style_border_color(r->head, lv_color_black(), 0);
		r->head_name = ui_label(r->head, FONT_MEDIUM);
		lv_obj_align(r->head_name, LV_ALIGN_LEFT_MID, 2, 0);
		r->head_state = ui_label(r->head, FONT_SMALL);
		lv_obj_align(r->head_state, LV_ALIGN_RIGHT_MID, -3, 0);

		/* Channel */
		r->chan = ui_box(padlist_view, 6, y, LIST_ROW_W, LIST_ROW_H - 1);
		r->ch = ui_label(r->chan, FONT_SMALL);
		lv_obj_align(r->ch, LV_ALIGN_LEFT_MID, 22, 0);
		r->name = ui_label(r->chan, FONT_MEDIUM);
		lv_obj_set_width(r->name, 90);
		lv_label_set_long_mode(r->name, LV_LABEL_LONG_MODE_CLIP);
		lv_obj_align(r->name, LV_ALIGN_LEFT_MID, 66, 0);
		r->role = ui_label(r->chan, FONT_SMALL);
		lv_obj_align(r->role, LV_ALIGN_LEFT_MID, 162, 0);
	}
	padlist_bar = ui_scrollbar(padlist_view, LIST_Y0, LIST_H);

	padlist_empty = ui_text_line(padlist_view, FONT_MEDIUM, SCREEN_W - 2 * MARGIN,
				     LV_TEXT_ALIGN_CENTER);
	lv_obj_set_pos(padlist_empty, MARGIN, LIST_Y0 + LIST_H / 2 - 12);
	ui_set_text(padlist_empty, "Pair a satellite first");

	ui_footer(padlist_view, LIST_RULE_Y, LV_SYMBOL_UP LV_SYMBOL_DOWN " channel  "
		  LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " satellite  OK: set up  BACK: back");
}

static void create_setup(lv_obj_t *scr)
{
	int32_t bottom = SETUP_Y0 + FIELD_COUNT * SETUP_H;

	setup_view = ui_view(scr, false);
	setup_title = ui_title(setup_view, "");

	for (int f = 0; f < FIELD_COUNT; f++) {
		lv_obj_t *name;

		setup_row[f] = ui_box(setup_view, ROW_X, SETUP_Y0 + f * SETUP_H, SCREEN_W - 36,
				      SETUP_H - 2);

		name = ui_label(setup_row[f], FONT_SMALL);
		ui_set_text(name, field_names[f]);
		lv_obj_align(name, LV_ALIGN_LEFT_MID, 8, 0);

		setup_value[f] = ui_label(setup_row[f], FONT_MEDIUM);
		lv_obj_align(setup_value[f], LV_ALIGN_LEFT_MID, 110, 0);
	}

	/* Character boxes over the label value, shown while editing it */
	for (int i = 0; i < PAD_LABEL_MAX; i++) {
		setup_char_box[i] = ui_box(setup_row[FIELD_LABEL], 106 + i * 19, 1, 18,
					   SETUP_H - 4);
		lv_obj_add_style(setup_char_box[i], &ui_style_frame_thin, 0);
		setup_char[i] = ui_label(setup_char_box[i], FONT_MEDIUM);
		lv_obj_center(setup_char[i]);
	}

	/* Two lines under the fields: what the buttons do, and what is wrong */
	ui_rule(setup_view, bottom + 1);
	setup_help = ui_label(setup_view, FONT_SMALL);
	lv_obj_set_pos(setup_help, MARGIN, bottom + 7);
	setup_error = ui_text_line(setup_view, FONT_SMALL, SCREEN_W - 2 * MARGIN,
				   LV_TEXT_ALIGN_LEFT);
	lv_obj_set_pos(setup_error, MARGIN, bottom + 24);
}

void ui_pads_create(lv_obj_t *scr)
{
	create_padlist(scr);
	create_setup(scr);
}

/* What a pad does with a channel */
static const char *role_text(const struct pad_cfg *cfg, int ch)
{
	if (cfg->type == PAD_ROCKET) {
		return "rocket";
	}
	if (ch == cfg->fill) {
		return "hybrid fill";
	}
	return ch == cfg->vent ? "hybrid vent" : "hybrid igniter";
}

/* row 0 is the satellite's header, the others its channels */
static void refresh_chan_row(const struct chan_row *r, const struct ctrl_view *v, int sat,
			     int row)
{
	ui_set_visible(r->head, row == 0);
	ui_set_visible(r->chan, row > 0);

	if (row == 0) {
		ui_set_text_fmt(r->head_name, "%c", sat_name(sat));
		ui_set_text(r->head_state, v->sat[sat].uid ? "paired" : "not paired");
	} else {
		int ch = row - 1;
		int p = ctrl_channel_pad(sat, ch);

		ui_set_text_fmt(r->ch, "CH%d", ch + 1);
		ui_set_text(r->name, p >= 0 ? pad_name(p) : "free");
		ui_set_text(r->role, p >= 0 ? role_text(&pads[p], ch) : "");
	}
}

lv_obj_t *ui_padlist_refresh(const struct ctrl_view *v)
{
	int sats[NUM_SATS];
	int n = ctrl_pads_sats(sats);
	int count = n * ROWS_PER_SAT;
	/* Its row: the satellite's header, then one per channel */
	int cursor = n ? (v->pad_list.cursor / NUM_CHANNELS) * ROWS_PER_SAT + 1 +
			 v->pad_list.cursor % NUM_CHANNELS : -1;
	/* The header too, on a satellite's first channel */
	int top = cursor - (v->pad_list.cursor % NUM_CHANNELS == 0);

	padlist_first = ui_scroll(padlist_first, top, cursor, count, LIST_ROWS);

	for (int i = 0; i < LIST_ROWS; i++) {
		const struct chan_row *r = &padlist_rows[i];
		int k = padlist_first + i;

		if (k >= count) {
			ui_set_visible(r->head, false);
			ui_set_visible(r->chan, false);
			continue;
		}
		refresh_chan_row(r, v, sats[k / ROWS_PER_SAT], k % ROWS_PER_SAT);
		ui_set_inverted(r->chan, k == cursor);
	}
	ui_scrollbar_set(padlist_bar, LIST_Y0, LIST_H, padlist_first, count, LIST_ROWS);
	ui_set_visible(padlist_empty, n == 0);
	return padlist_view;
}

static void field_text(const struct pad_cfg *cfg, int field, char *buf, size_t len)
{
	switch (field) {
	case FIELD_LABEL:
		snprintf(buf, len, "%s", cfg->label[0] ? cfg->label : "-");
		break;
	case FIELD_TYPE:
		snprintf(buf, len, "%s", pad_type_name(cfg->type));
		break;
	case FIELD_SAT:
		snprintf(buf, len, "%c", sat_name(cfg->sat));
		break;
	case FIELD_IGN:
		snprintf(buf, len, "CH%d", cfg->ign + 1);
		break;
	case FIELD_FILL:
		snprintf(buf, len, "CH%d", cfg->fill + 1);
		break;
	case FIELD_VENT:
		snprintf(buf, len, "CH%d", cfg->vent + 1);
		break;
	}
}

/* Off pads have a label and a type only, rockets no valves */
static bool field_used(const struct pad_cfg *cfg, int field)
{
	switch (field) {
	case FIELD_LABEL:
	case FIELD_TYPE:
		return true;
	case FIELD_SAT:
	case FIELD_IGN:
		return cfg->type != PAD_OFF;
	default:
		return cfg->type == PAD_HYBRID;
	}
}

static void refresh_fields(const struct pad_setup *su, bool editing_label)
{
	char buf[24];

	for (int f = 0; f < FIELD_COUNT; f++) {
		bool focused = f == su->field && !editing_label;

		ui_set_visible(setup_row[f], field_used(&su->cfg, f));
		if (!field_used(&su->cfg, f)) {
			continue;
		}
		ui_set_inverted(setup_row[f], focused);

		field_text(&su->cfg, f, buf, sizeof(buf));
		if (focused && f != FIELD_LABEL) {
			/* Arrows show that left and right change the value */
			ui_set_text_fmt(setup_value[f], LV_SYMBOL_LEFT "  %s  " LV_SYMBOL_RIGHT, buf);
		} else {
			ui_set_text(setup_value[f], buf);
		}
	}
}

/* While editing, the label is shown one character per box */
static void refresh_label_editor(const struct pad_setup *su, bool editing_label)
{
	ui_set_visible(setup_value[FIELD_LABEL], !editing_label);
	for (int i = 0; i < PAD_LABEL_MAX; i++) {
		char c[2] = {su->label[i], '\0'};

		ui_set_visible(setup_char_box[i], editing_label);
		if (editing_label) {
			ui_set_text(setup_char[i], c);
			ui_set_inverted(setup_char_box[i], i == su->pos);
		}
	}
}

lv_obj_t *ui_setup_refresh(const struct ctrl_view *v)
{
	static const char *const help[] = {
		[SETUP_FIELDS] = LV_SYMBOL_UP LV_SYMBOL_DOWN " field  " LV_SYMBOL_LEFT LV_SYMBOL_RIGHT
				 " value  Hold OK: save  BACK: cancel",
		[SETUP_LABEL] = LV_SYMBOL_UP LV_SYMBOL_DOWN " letter  " LV_SYMBOL_LEFT LV_SYMBOL_RIGHT
				" position  OK: done  BACK: undo",
	};
	const struct pad_setup *su = &v->setup;
	bool editing_label = su->mode == SETUP_LABEL;

	if (su->is_new) {
		ui_set_text_fmt(setup_title, "NEW PAD %d  -  %c CH%d", su->pad + 1,
				sat_name(su->cfg.sat), su->cfg.ign + 1);
	} else {
		ui_set_text_fmt(setup_title, "PAD %d SETUP", su->pad + 1);
	}
	refresh_fields(su, editing_label);
	refresh_label_editor(su, editing_label);

	ui_set_text(setup_help, help[su->mode]);
	if (su->error) {
		ui_set_text_fmt(setup_error, LV_SYMBOL_WARNING " %s", su->error);
	} else {
		ui_set_text(setup_error, "");
	}
	return setup_view;
}
