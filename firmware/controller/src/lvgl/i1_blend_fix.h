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
 */

#ifndef I1_BLEND_FIX_H_
#define I1_BLEND_FIX_H_

/* LVGL includes this from every draw unit; only the blenders can use it */
#ifdef LV_DRAW_SW_BLEND_PRIVATE_H

static inline void i1_put(uint8_t *row, int32_t x, bool on)
{
	uint8_t bit = 0x80 >> (x % 8);

	if (on) {
		row[x / 8] |= bit;
	} else {
		row[x / 8] &= ~bit;
	}
}

static inline bool i1_get(const uint8_t *row, int32_t x)
{
	return row[x / 8] & (0x80 >> (x % 8));
}

static inline lv_result_t i1_blend_color_masked(lv_draw_sw_blend_fill_dsc_t *dsc)
{
	const lv_opa_t *mask = dsc->mask_buf;
	uint8_t *row = dsc->dest_buf;
	int32_t ofs = dsc->relative_area.x1 % 8;
	bool src = lv_color_luminance(dsc->color) > LV_DRAW_SW_I1_LUM_THRESHOLD;

	for (int32_t y = 0; y < dsc->dest_h; y++) {
		for (int32_t x = 0; x < dsc->dest_w; x++) {
			uint32_t cover = dsc->opa >= LV_OPA_MAX ? mask[x]
								: (uint32_t)mask[x] * dsc->opa / 255;

			if (cover == LV_OPA_TRANSP) {
				continue;
			}
			/* Blend in 0..255 and threshold, instead of 0/1 and truncate */
			uint32_t dest = i1_get(row, x + ofs) ? 255 : 0;
			uint32_t lum = (cover * (src ? 255 : 0) + (255 - cover) * dest) / 255;

			i1_put(row, x + ofs, lum > LV_DRAW_SW_I1_LUM_THRESHOLD);
		}
		row += dsc->dest_stride;
		mask += dsc->mask_stride;
	}
	return LV_RESULT_OK;
}

#define LV_DRAW_SW_COLOR_BLEND_TO_I1_WITH_MASK(dsc)   i1_blend_color_masked(dsc)
#define LV_DRAW_SW_COLOR_BLEND_TO_I1_MIX_MASK_OPA(dsc) i1_blend_color_masked(dsc)

#endif /* LV_DRAW_SW_BLEND_PRIVATE_H */
#endif /* I1_BLEND_FIX_H_ */
