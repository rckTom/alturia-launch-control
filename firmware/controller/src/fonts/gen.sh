#!/bin/sh
# Regenerates the UI fonts: Noto Sans Condensed Bold plus the few LVGL
# symbols the UI uses, rendered at 1 bpp. The panel has no grey, so
# anti-aliased glyphs would only be thresholded into ragged strokes, and a
# bold face is what keeps them readable. Condensed keeps the larger sizes
# inside the fixed layout columns.
#
# Needs node (npx fetches lv_font_conv) and the Noto Sans and FontAwesome
# fonts; override TTF / SYMBOLS if they live elsewhere.
set -e
cd "$(dirname "$0")"

TTF=${TTF:-/usr/share/fonts/noto/NotoSans-CondensedBold.ttf}
SYMBOLS=${SYMBOLS:-$ZEPHYR_BASE/../modules/lib/gui/lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff}

# LV_SYMBOL_WARNING, LEFT, RIGHT, UP, DOWN
SYMBOL_RANGE=0xF071,0xF053,0xF054,0xF077,0xF078

for size in 18 20 28; do
	npx --yes lv_font_conv --no-compress --bpp 1 --format lvgl \
		--size "$size" --lv-font-name "ui_font_$size" --lv-include lvgl.h \
		--font "$TTF" -r 0x20-0x7E \
		--font "$SYMBOLS" -r "$SYMBOL_RANGE" \
		-o "ui_font_$size.c"
done
