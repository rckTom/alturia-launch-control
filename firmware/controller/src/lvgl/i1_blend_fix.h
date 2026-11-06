/*
 * Replacement for LVGL's masked color fill into 1-bit (I1) buffers,
 * hooked in through LV_DRAW_SW_ASM_CUSTOM (see prj.conf).
 *
 * LVGL 9.3 blends anti-aliased pixels with
 *     new = (mask * src + (255 - mask) * dest) / 255
 * which truncates. Black text on white gains every partly covered pixel,
 * but white text on black keeps only fully covered ones, so inverted text
 * falls apart. This version thresholds the blended coverage at 50 %, which
 * treats both polarities the same.
 *
 * LVGL includes this header from lv_conf_internal.h and lv_draw_sw_blend.h,
 * both before the blend descriptor is defined, so only the hooks live here;
 * the blender itself is in i1_blend_fix.c.
 */

#ifndef I1_BLEND_FIX_H_
#define I1_BLEND_FIX_H_

struct _lv_draw_sw_blend_fill_dsc_t;

/* Returns an lv_result_t; that type isn't declared yet either */
int i1_blend_color_masked(struct _lv_draw_sw_blend_fill_dsc_t *dsc);

#define LV_DRAW_SW_COLOR_BLEND_TO_I1_WITH_MASK(dsc)   i1_blend_color_masked(dsc)
#define LV_DRAW_SW_COLOR_BLEND_TO_I1_MIX_MASK_OPA(dsc) i1_blend_color_masked(dsc)

#endif /* I1_BLEND_FIX_H_ */
