/*
 * Pads: all pads including OFF ones, with their channels. The main list
 * only shows enabled pads.
 *
 * Pad setup: label, type, satellite and channels of one pad. The focused
 * field is inverted; the label is edited in one box per character.
 */

#include <stdio.h>

#include "views.h"
#include "widgets.h"

/* Pad list */
enum { COL_NUM, COL_LABEL, COL_TYPE, COL_SAT, COL_CHANNELS, COL_COUNT };

static const int32_t col_x[COL_COUNT] = {6, 28, 90, 160, 184};

static lv_obj_t *padlist_view;
static lv_obj_t *padlist_col[NUM_PADS][COL_COUNT];
static lv_obj_t *padlist_cursor;

/* Pad setup */
#define SETUP_Y0 33
#define SETUP_H  26

static const char *const field_names[FIELD_COUNT] = {
	"Label", "Type", "Satellite", "Igniter", "Fill valve", "Vent valve",
};

static lv_obj_t *setup_view;
static lv_obj_t *setup_title;
static lv_obj_t *setup_cursor;
static lv_obj_t *setup_row[FIELD_COUNT];
static lv_obj_t *setup_value[FIELD_COUNT];
static lv_obj_t *setup_char_box[PAD_LABEL_MAX];
static lv_obj_t *setup_char[PAD_LABEL_MAX];
static lv_obj_t *setup_help;
static lv_obj_t *setup_error;

static void create_padlist(lv_obj_t *scr)
{
	padlist_view = ui_view(scr, false);
	ui_title(padlist_view, "SETTINGS  -  PADS");

	for (int p = 0; p < NUM_PADS; p++) {
		lv_obj_t *row = ui_box(padlist_view, ROW_X, PAD_ROW_Y0 + p * PAD_ROW_H,
				       SCREEN_W - 22, PAD_ROW_H - 2);

		for (int c = 0; c < COL_COUNT; c++) {
			padlist_col[p][c] = ui_label(row, FONT_SMALL);
			lv_obj_align(padlist_col[p][c], LV_ALIGN_LEFT_MID, col_x[c], 0);
		}
		lv_obj_set_width(padlist_col[p][COL_LABEL], 60);
		lv_label_set_long_mode(padlist_col[p][COL_LABEL], LV_LABEL_LONG_MODE_CLIP);
	}
	padlist_cursor = ui_cursor(padlist_view);

	ui_footer(padlist_view, PAD_ROW_Y0 + NUM_PADS * PAD_ROW_H + 1,
		  LV_SYMBOL_UP LV_SYMBOL_DOWN " pad    OK: set up    BACK: back");
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
	setup_cursor = ui_cursor(setup_view);

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

lv_obj_t *ui_padlist_refresh(const struct ctrl_view *v)
{
	for (int p = 0; p < NUM_PADS; p++) {
		const struct pad_cfg *cfg = &pads[p];
		lv_obj_t **col = padlist_col[p];

		ui_set_text_fmt(col[COL_NUM], "%d", p + 1);
		ui_set_text(col[COL_LABEL], cfg->label[0] ? cfg->label : "-");
		ui_set_text(col[COL_TYPE], pad_type_name(cfg->type));

		switch (cfg->type) {
		case PAD_HYBRID:
			ui_set_text_fmt(col[COL_SAT], "%c", sat_name(cfg->sat));
			ui_set_text_fmt(col[COL_CHANNELS], "IGN %d  FILL %d  VENT %d",
					cfg->ign + 1, cfg->fill + 1, cfg->vent + 1);
			break;
		case PAD_ROCKET:
			ui_set_text_fmt(col[COL_SAT], "%c", sat_name(cfg->sat));
			ui_set_text_fmt(col[COL_CHANNELS], "IGN %d", cfg->ign + 1);
			break;
		default:
			ui_set_text(col[COL_SAT], "");
			ui_set_text(col[COL_CHANNELS], "");
			break;
		}
	}
	lv_obj_set_pos(padlist_cursor, CURSOR_X, PAD_ROW_Y0 + v->pad_list.cursor * PAD_ROW_H + 3);
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

	ui_set_text_fmt(setup_title, "PAD %d SETUP", su->pad + 1);
	refresh_fields(su, editing_label);
	refresh_label_editor(su, editing_label);

	lv_obj_set_pos(setup_cursor, CURSOR_X, SETUP_Y0 + su->field * SETUP_H + 4);
	ui_set_text(setup_help, help[su->mode]);
	if (su->error) {
		ui_set_text_fmt(setup_error, LV_SYMBOL_WARNING " %s", su->error);
	} else {
		ui_set_text(setup_error, "");
	}
	return setup_view;
}
