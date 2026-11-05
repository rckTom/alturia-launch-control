/*******************************************************************************
 * Size: 18 px
 * Bpp: 1
 * Opts: --no-compress --bpp 1 --format lvgl --size 18 --lv-font-name ui_font_18 --lv-include lvgl.h --font /usr/share/fonts/noto/NotoSans-CondensedBold.ttf -r 0x20-0x7E --font $ZEPHYR_BASE/../modules/lib/gui/lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff -r 0xF071,0xF053,0xF054,0xF077,0xF078 -o ui_font_18.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef UI_FONT_18
#define UI_FONT_18 1
#endif

#if UI_FONT_18

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0020 " " */
    0x0,

    /* U+0021 "!" */
    0xff, 0xff, 0xf, 0xc0,

    /* U+0022 "\"" */
    0xcf, 0x3c, 0xf3,

    /* U+0023 "#" */
    0x1b, 0xd, 0x8c, 0xc6, 0x6f, 0xff, 0xfc, 0xd8,
    0x6c, 0xff, 0xff, 0xd9, 0x8d, 0x86, 0xc0,

    /* U+0024 "$" */
    0x10, 0x7b, 0xfe, 0x8d, 0x1e, 0xf, 0xf, 0x17,
    0x2f, 0xfb, 0xe1, 0x2, 0x0,

    /* U+0025 "%" */
    0x70, 0xc3, 0xe3, 0xd, 0x98, 0x36, 0x60, 0xdb,
    0x3, 0x6d, 0xef, 0xbf, 0x9d, 0xb3, 0x6, 0xcc,
    0x33, 0x30, 0xcc, 0xc6, 0x1e, 0x18, 0x78,

    /* U+0026 "&" */
    0x38, 0x3e, 0x1b, 0xd, 0x87, 0xc1, 0xc1, 0xcf,
    0xf7, 0xdf, 0x67, 0xb3, 0x9f, 0xe7, 0xb8,

    /* U+0027 "'" */
    0xff,

    /* U+0028 "(" */
    0x36, 0x66, 0xcc, 0xcc, 0xcc, 0xc6, 0x67, 0x30,

    /* U+0029 ")" */
    0xce, 0x66, 0x33, 0x33, 0x33, 0x36, 0x6e, 0xc0,

    /* U+002A "*" */
    0x18, 0x18, 0x7b, 0x7f, 0x1c, 0x3e, 0x76, 0x4,

    /* U+002B "+" */
    0x18, 0x18, 0x18, 0xff, 0xff, 0x18, 0x18, 0x18,

    /* U+002C "," */
    0xfb, 0x60,

    /* U+002D "-" */
    0xff, 0xc0,

    /* U+002E "." */
    0xff, 0x80,

    /* U+002F "/" */
    0x6, 0x1c, 0x30, 0x61, 0xc3, 0x6, 0x18, 0x30,
    0xe1, 0x83, 0xe, 0x0,

    /* U+0030 "0" */
    0x38, 0xfb, 0xb6, 0x3c, 0x78, 0xf1, 0xe3, 0xc7,
    0x8f, 0xbb, 0xe3, 0x80,

    /* U+0031 "1" */
    0x19, 0xdf, 0xf5, 0x8c, 0x63, 0x18, 0xc6, 0x31,
    0x80,

    /* U+0032 "2" */
    0x78, 0xfc, 0x46, 0x6, 0x6, 0xe, 0xc, 0x1c,
    0x38, 0x70, 0x60, 0xff, 0xff,

    /* U+0033 "3" */
    0x7d, 0xfd, 0x18, 0x30, 0x67, 0x8f, 0x7, 0x6,
    0xe, 0x3f, 0xef, 0x80,

    /* U+0034 "4" */
    0x6, 0xe, 0xe, 0x1e, 0x36, 0x36, 0x66, 0xe6,
    0xff, 0xff, 0x6, 0x6, 0x6,

    /* U+0035 "5" */
    0x7e, 0x7e, 0x70, 0x70, 0x70, 0x7e, 0x7e, 0x7,
    0x3, 0x3, 0x87, 0xfe, 0x7c,

    /* U+0036 "6" */
    0x1e, 0x3e, 0x60, 0x40, 0xdc, 0xfe, 0xe7, 0xc3,
    0xc3, 0xc3, 0xe7, 0x7e, 0x3c,

    /* U+0037 "7" */
    0xff, 0xff, 0x7, 0x6, 0xe, 0xe, 0xc, 0x1c,
    0x1c, 0x18, 0x38, 0x30, 0x70,

    /* U+0038 "8" */
    0x3c, 0x7e, 0x66, 0x66, 0x66, 0x7c, 0x3c, 0x7e,
    0xc3, 0xc3, 0xc3, 0xfe, 0x3c,

    /* U+0039 "9" */
    0x3c, 0x7e, 0xe6, 0xc3, 0xc3, 0xc7, 0x7f, 0x7b,
    0x3, 0x6, 0xe, 0x7c, 0x78,

    /* U+003A ":" */
    0xff, 0x80, 0x7, 0xfc,

    /* U+003B ";" */
    0xff, 0x80, 0x0, 0xff, 0x60,

    /* U+003C "<" */
    0x1, 0x7, 0x1e, 0x78, 0xe0, 0x78, 0x1e, 0x7,
    0x1,

    /* U+003D "=" */
    0xff, 0xff, 0x0, 0xff, 0xff,

    /* U+003E ">" */
    0x80, 0xe0, 0x78, 0x1e, 0x7, 0x1e, 0x78, 0xe0,
    0x80,

    /* U+003F "?" */
    0x7c, 0xfd, 0x18, 0x30, 0xe3, 0x8e, 0x18, 0x30,
    0x0, 0xe1, 0xc3, 0x80,

    /* U+0040 "@" */
    0xf, 0x80, 0xff, 0xe, 0x1c, 0xcf, 0x66, 0xf9,
    0xee, 0xcf, 0x66, 0x7b, 0x33, 0xd9, 0x9e, 0xf7,
    0xb3, 0xb8, 0xc0, 0x7, 0x4, 0x1f, 0xe0, 0x7e,
    0x0,

    /* U+0041 "A" */
    0x1e, 0x7, 0x81, 0xe0, 0x7c, 0x3b, 0xe, 0xc3,
    0x39, 0xce, 0x7f, 0x9f, 0xe6, 0x1f, 0x87, 0xe1,
    0xc0,

    /* U+0042 "B" */
    0xfc, 0x7f, 0xb0, 0xd8, 0x6c, 0x37, 0xf3, 0xfd,
    0x8f, 0xc3, 0xe1, 0xf1, 0xff, 0xef, 0xe0,

    /* U+0043 "C" */
    0x1f, 0x3f, 0x61, 0xe0, 0xc0, 0xc0, 0xc0, 0xc0,
    0xc0, 0xe0, 0x61, 0x7f, 0x1f,

    /* U+0044 "D" */
    0xfc, 0x7f, 0x31, 0xd8, 0x7c, 0x1e, 0xf, 0x7,
    0x83, 0xc1, 0xe1, 0xf1, 0xdf, 0xcf, 0xc0,

    /* U+0045 "E" */
    0xff, 0xfc, 0x30, 0xc3, 0xff, 0xf0, 0xc3, 0xc,
    0x3f, 0xfc,

    /* U+0046 "F" */
    0xff, 0xfc, 0x30, 0xc3, 0xf, 0xff, 0xc3, 0xc,
    0x30, 0xc0,

    /* U+0047 "G" */
    0x1f, 0x1f, 0xdc, 0x1c, 0xc, 0x6, 0x3, 0x1f,
    0x8f, 0xc1, 0xf0, 0xd8, 0x6f, 0xf1, 0xf0,

    /* U+0048 "H" */
    0xc1, 0xe0, 0xf0, 0x78, 0x3c, 0x1f, 0xff, 0xff,
    0x83, 0xc1, 0xe0, 0xf0, 0x78, 0x3c, 0x18,

    /* U+0049 "I" */
    0xff, 0xcc, 0x63, 0x18, 0xc6, 0x31, 0x8d, 0xff,
    0x80,

    /* U+004A "J" */
    0x18, 0xc6, 0x31, 0x8c, 0x63, 0x18, 0xc6, 0x31,
    0x8f, 0xde,

    /* U+004B "K" */
    0xc7, 0xc6, 0xce, 0xdc, 0xd8, 0xf8, 0xf8, 0xfc,
    0xdc, 0xce, 0xce, 0xc7, 0xc7,

    /* U+004C "L" */
    0xc1, 0x83, 0x6, 0xc, 0x18, 0x30, 0x60, 0xc1,
    0x83, 0x7, 0xff, 0xe0,

    /* U+004D "M" */
    0xe0, 0x7f, 0xf, 0xf0, 0xff, 0xf, 0xf9, 0xfd,
    0x9b, 0xd9, 0xbd, 0x9b, 0xdf, 0x3c, 0xf3, 0xcf,
    0x3c, 0xe3, 0xc6, 0x30,

    /* U+004E "N" */
    0xe1, 0xf0, 0xfc, 0x7e, 0x3f, 0x9e, 0xcf, 0x77,
    0x9b, 0xcf, 0xe3, 0xf1, 0xf8, 0x7c, 0x38,

    /* U+004F "O" */
    0x1e, 0x1f, 0xe6, 0x1b, 0x87, 0xc0, 0xf0, 0x3c,
    0xf, 0x3, 0xc0, 0xf8, 0x76, 0x19, 0xfe, 0x1e,
    0x0,

    /* U+0050 "P" */
    0xf9, 0xfb, 0x3e, 0x3c, 0x79, 0xff, 0x7c, 0xc1,
    0x83, 0x6, 0xc, 0x0,

    /* U+0051 "Q" */
    0x1e, 0x1f, 0xe6, 0x1b, 0x87, 0xc0, 0xf0, 0x3c,
    0xf, 0x3, 0xc0, 0xf8, 0x76, 0x18, 0xfe, 0x1f,
    0x0, 0xe0, 0x38, 0x7,

    /* U+0052 "R" */
    0xf8, 0xfc, 0xce, 0xc6, 0xc6, 0xce, 0xfc, 0xfc,
    0xdc, 0xcc, 0xce, 0xc6, 0xc7,

    /* U+0053 "S" */
    0x3c, 0xff, 0x6, 0xe, 0x1e, 0x1f, 0xf, 0xe,
    0xe, 0x1f, 0xef, 0x80,

    /* U+0054 "T" */
    0xff, 0xff, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18, 0x18, 0x18,

    /* U+0055 "U" */
    0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3,
    0xc3, 0xc3, 0xe7, 0x7e, 0x3c,

    /* U+0056 "V" */
    0xe1, 0xf8, 0x66, 0x19, 0x86, 0x73, 0x9c, 0xc3,
    0x30, 0xcc, 0x3f, 0x7, 0x81, 0xe0, 0x78, 0x1c,
    0x0,

    /* U+0057 "W" */
    0xc3, 0x8f, 0xc7, 0x1b, 0x8e, 0x33, 0x3c, 0x66,
    0x79, 0xcc, 0xdb, 0x9d, 0xb6, 0x3b, 0x6c, 0x3c,
    0xd8, 0x79, 0xf0, 0xf1, 0xe1, 0xe3, 0x83, 0xc7,
    0x0,

    /* U+0058 "X" */
    0x63, 0x9c, 0xe7, 0x70, 0xdc, 0x3e, 0x7, 0x81,
    0xc0, 0x78, 0x3e, 0xd, 0xc7, 0x31, 0x8e, 0xe1,
    0x80,

    /* U+0059 "Y" */
    0xc3, 0xe7, 0x66, 0x66, 0x7c, 0x3c, 0x3c, 0x18,
    0x18, 0x18, 0x18, 0x18, 0x18,

    /* U+005A "Z" */
    0xff, 0xff, 0x7, 0xe, 0xc, 0x1c, 0x18, 0x38,
    0x30, 0x70, 0xe0, 0xff, 0xff,

    /* U+005B "[" */
    0xff, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xff,

    /* U+005C "\\" */
    0xe0, 0xc1, 0x83, 0x83, 0x6, 0x6, 0xc, 0x1c,
    0x18, 0x30, 0x70, 0x60,

    /* U+005D "]" */
    0xff, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0xff,

    /* U+005E "^" */
    0xc, 0xe, 0x7, 0x83, 0xc3, 0x31, 0x99, 0x86,
    0xc3,

    /* U+005F "_" */
    0xfe,

    /* U+0060 "`" */
    0xe6, 0x30,

    /* U+0061 "a" */
    0x7c, 0xfd, 0x18, 0x37, 0xff, 0xf1, 0xe3, 0xfe,
    0xec,

    /* U+0062 "b" */
    0xc0, 0xc0, 0xc0, 0xc0, 0xdc, 0xfe, 0xe7, 0xc3,
    0xc3, 0xc3, 0xc3, 0xe7, 0xfe, 0xdc,

    /* U+0063 "c" */
    0x3c, 0xfb, 0x86, 0xc, 0x18, 0x30, 0x71, 0x7e,
    0x78,

    /* U+0064 "d" */
    0x3, 0x3, 0x3, 0x3, 0x3b, 0x7f, 0xe7, 0xc3,
    0xc3, 0xc3, 0xc3, 0xe7, 0x7f, 0x3b,

    /* U+0065 "e" */
    0x38, 0xfb, 0x1e, 0x3f, 0xff, 0xf0, 0x71, 0x7e,
    0x78,

    /* U+0066 "f" */
    0x3e, 0xf9, 0x83, 0xf, 0xdf, 0x98, 0x30, 0x60,
    0xc1, 0x83, 0x6, 0xc, 0x0,

    /* U+0067 "g" */
    0x3b, 0x7f, 0xe7, 0xc3, 0xc3, 0xc3, 0xc3, 0xe7,
    0x7f, 0x3b, 0x3, 0x87, 0xfe, 0x7c,

    /* U+0068 "h" */
    0xc1, 0x83, 0x6, 0xd, 0xdf, 0xf9, 0xe3, 0xc7,
    0x8f, 0x1e, 0x3c, 0x78, 0xc0,

    /* U+0069 "i" */
    0xf0, 0xff, 0xff, 0xf0,

    /* U+006A "j" */
    0x33, 0x0, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33,
    0xfe,

    /* U+006B "k" */
    0xc0, 0xc0, 0xc0, 0xc0, 0xc6, 0xce, 0xdc, 0xd8,
    0xf8, 0xf8, 0xdc, 0xce, 0xce, 0xc7,

    /* U+006C "l" */
    0xff, 0xff, 0xff, 0xf0,

    /* U+006D "m" */
    0xdc, 0xef, 0xff, 0xe7, 0x3c, 0x63, 0xc6, 0x3c,
    0x63, 0xc6, 0x3c, 0x63, 0xc6, 0x3c, 0x63,

    /* U+006E "n" */
    0xdd, 0xff, 0x9e, 0x3c, 0x78, 0xf1, 0xe3, 0xc7,
    0x8c,

    /* U+006F "o" */
    0x3c, 0x7e, 0xe7, 0xc3, 0xc3, 0xc3, 0xc3, 0xe7,
    0x7e, 0x3c,

    /* U+0070 "p" */
    0xdc, 0xfe, 0xe7, 0xc3, 0xc3, 0xc3, 0xc3, 0xe7,
    0xfe, 0xdc, 0xc0, 0xc0, 0xc0, 0xc0,

    /* U+0071 "q" */
    0x3b, 0x7f, 0xe7, 0xc3, 0xc3, 0xc3, 0xc3, 0xe7,
    0x7f, 0x3b, 0x3, 0x3, 0x3, 0x3,

    /* U+0072 "r" */
    0xdf, 0xf9, 0x8c, 0x63, 0x18, 0xc6, 0x0,

    /* U+0073 "s" */
    0x7b, 0xfc, 0x38, 0x70, 0xf0, 0xe3, 0xff, 0xe0,

    /* U+0074 "t" */
    0x21, 0x8f, 0xff, 0x61, 0x86, 0x18, 0x61, 0x87,
    0xcf,

    /* U+0075 "u" */
    0xc7, 0x8f, 0x1e, 0x3c, 0x78, 0xf1, 0xe7, 0xfe,
    0xec,

    /* U+0076 "v" */
    0xe3, 0x71, 0x99, 0xcc, 0xe7, 0x61, 0xb0, 0xf8,
    0x78, 0x1c, 0xe, 0x0,

    /* U+0077 "w" */
    0xe7, 0x1f, 0x39, 0xd9, 0x4c, 0xcb, 0x67, 0xdb,
    0x3e, 0xf8, 0xf7, 0xc7, 0x9c, 0x38, 0xe1, 0xc7,
    0x0,

    /* U+0078 "x" */
    0xe7, 0x77, 0x7e, 0x3e, 0x3c, 0x3c, 0x3e, 0x76,
    0x67, 0xe7,

    /* U+0079 "y" */
    0x63, 0xb1, 0xdc, 0xc6, 0x63, 0x71, 0xb0, 0xf8,
    0x3c, 0x1c, 0xe, 0x7, 0x3, 0x7, 0x83, 0x80,

    /* U+007A "z" */
    0xff, 0xf1, 0xc6, 0x38, 0xc7, 0x18, 0xff, 0xf0,

    /* U+007B "{" */
    0x1c, 0xf3, 0xc, 0x30, 0xc3, 0x38, 0xe0, 0xc3,
    0xc, 0x30, 0xc3, 0xc7,

    /* U+007C "|" */
    0xff, 0xff, 0xff, 0xff, 0xf0,

    /* U+007D "}" */
    0xe3, 0xc3, 0xc, 0x30, 0xc3, 0x7, 0x1c, 0xc3,
    0xc, 0x30, 0xcf, 0x38,

    /* U+007E "~" */
    0xf1, 0xff, 0x8f,

    /* U+F053 "" */
    0x1, 0x81, 0xc1, 0xe1, 0xe1, 0xe1, 0xe1, 0xe1,
    0xe0, 0x78, 0x1e, 0x7, 0x81, 0xe0, 0x78, 0x1c,
    0x6,

    /* U+F054 "" */
    0x40, 0x70, 0x1e, 0x7, 0x1, 0xe0, 0x78, 0x1e,
    0x7, 0x7, 0x87, 0x87, 0x87, 0x87, 0x7, 0x1,
    0x0,

    /* U+F071 "" */
    0x0, 0x60, 0x0, 0x7, 0x80, 0x0, 0x3e, 0x0,
    0x3, 0xf0, 0x0, 0x1f, 0xc0, 0x1, 0xfe, 0x0,
    0x1e, 0x38, 0x0, 0xf1, 0xc0, 0xf, 0x8f, 0x0,
    0x7c, 0x7c, 0x7, 0xe3, 0xe0, 0x7f, 0x1f, 0x83,
    0xff, 0xfc, 0x3f, 0xcf, 0xf1, 0xfe, 0x3f, 0x9f,
    0xf3, 0xfe, 0xff, 0xff, 0xf7, 0xff, 0xff, 0x80,

    /* U+F077 "" */
    0x3, 0x80, 0xf, 0x80, 0x3f, 0x80, 0xf7, 0x83,
    0xc7, 0x8f, 0x7, 0xbc, 0x7, 0xf0, 0x7, 0x40,
    0x4,

    /* U+F078 "" */
    0xe0, 0xf, 0xe0, 0x3d, 0xe0, 0xf1, 0xe3, 0xc1,
    0xef, 0x1, 0xfc, 0x1, 0xf0, 0x1, 0xc0, 0x1,
    0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 59, .box_w = 1, .box_h = 1, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1, .adv_w = 76, .box_w = 2, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 5, .adv_w = 131, .box_w = 6, .box_h = 4, .ofs_x = 1, .ofs_y = 9},
    {.bitmap_index = 8, .adv_w = 157, .box_w = 9, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 23, .adv_w = 143, .box_w = 7, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 36, .adv_w = 234, .box_w = 14, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 59, .adv_w = 177, .box_w = 9, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 74, .adv_w = 70, .box_w = 2, .box_h = 4, .ofs_x = 1, .ofs_y = 9},
    {.bitmap_index = 75, .adv_w = 93, .box_w = 4, .box_h = 15, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 83, .adv_w = 93, .box_w = 4, .box_h = 15, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 91, .adv_w = 137, .box_w = 8, .box_h = 8, .ofs_x = 0, .ofs_y = 6},
    {.bitmap_index = 99, .adv_w = 143, .box_w = 8, .box_h = 8, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 107, .adv_w = 74, .box_w = 3, .box_h = 4, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 109, .adv_w = 89, .box_w = 5, .box_h = 2, .ofs_x = 1, .ofs_y = 4},
    {.bitmap_index = 111, .adv_w = 76, .box_w = 3, .box_h = 3, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 113, .adv_w = 117, .box_w = 7, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 125, .adv_w = 142, .box_w = 7, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 137, .adv_w = 142, .box_w = 5, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 146, .adv_w = 142, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 159, .adv_w = 142, .box_w = 7, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 171, .adv_w = 142, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 184, .adv_w = 142, .box_w = 8, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 197, .adv_w = 142, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 210, .adv_w = 142, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 223, .adv_w = 142, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 236, .adv_w = 142, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 249, .adv_w = 77, .box_w = 3, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 253, .adv_w = 77, .box_w = 3, .box_h = 12, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 258, .adv_w = 143, .box_w = 8, .box_h = 9, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 267, .adv_w = 143, .box_w = 8, .box_h = 5, .ofs_x = 1, .ofs_y = 4},
    {.bitmap_index = 272, .adv_w = 143, .box_w = 8, .box_h = 9, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 281, .adv_w = 116, .box_w = 7, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 293, .adv_w = 223, .box_w = 13, .box_h = 15, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 318, .adv_w = 166, .box_w = 10, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 335, .adv_w = 164, .box_w = 9, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 350, .adv_w = 153, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 363, .adv_w = 176, .box_w = 9, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 378, .adv_w = 136, .box_w = 6, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 388, .adv_w = 134, .box_w = 6, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 398, .adv_w = 176, .box_w = 9, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 413, .adv_w = 185, .box_w = 9, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 428, .adv_w = 97, .box_w = 5, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 437, .adv_w = 84, .box_w = 5, .box_h = 16, .ofs_x = -1, .ofs_y = -3},
    {.bitmap_index = 447, .adv_w = 161, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 460, .adv_w = 130, .box_w = 7, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 472, .adv_w = 237, .box_w = 12, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 492, .adv_w = 197, .box_w = 9, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 507, .adv_w = 188, .box_w = 10, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 524, .adv_w = 152, .box_w = 7, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 536, .adv_w = 188, .box_w = 10, .box_h = 16, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 556, .adv_w = 160, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 569, .adv_w = 133, .box_w = 7, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 581, .adv_w = 137, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 594, .adv_w = 181, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 607, .adv_w = 158, .box_w = 10, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 624, .adv_w = 234, .box_w = 15, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 649, .adv_w = 153, .box_w = 10, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 666, .adv_w = 149, .box_w = 8, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 679, .adv_w = 133, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 692, .adv_w = 91, .box_w = 4, .box_h = 16, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 700, .adv_w = 117, .box_w = 7, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 712, .adv_w = 91, .box_w = 4, .box_h = 16, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 720, .adv_w = 156, .box_w = 9, .box_h = 8, .ofs_x = 0, .ofs_y = 5},
    {.bitmap_index = 729, .adv_w = 118, .box_w = 7, .box_h = 1, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 730, .adv_w = 94, .box_w = 4, .box_h = 3, .ofs_x = 1, .ofs_y = 11},
    {.bitmap_index = 732, .adv_w = 145, .box_w = 7, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 741, .adv_w = 155, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 755, .adv_w = 125, .box_w = 7, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 764, .adv_w = 155, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 778, .adv_w = 144, .box_w = 7, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 787, .adv_w = 94, .box_w = 7, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 800, .adv_w = 155, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 814, .adv_w = 157, .box_w = 7, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 827, .adv_w = 75, .box_w = 2, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 831, .adv_w = 75, .box_w = 4, .box_h = 18, .ofs_x = -1, .ofs_y = -4},
    {.bitmap_index = 840, .adv_w = 151, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 854, .adv_w = 75, .box_w = 2, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 858, .adv_w = 235, .box_w = 12, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 873, .adv_w = 157, .box_w = 7, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 882, .adv_w = 152, .box_w = 8, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 892, .adv_w = 155, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 906, .adv_w = 155, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 920, .adv_w = 109, .box_w = 5, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 927, .adv_w = 119, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 935, .adv_w = 104, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 944, .adv_w = 157, .box_w = 7, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 953, .adv_w = 139, .box_w = 9, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 965, .adv_w = 211, .box_w = 13, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 982, .adv_w = 135, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 992, .adv_w = 139, .box_w = 9, .box_h = 14, .ofs_x = 0, .ofs_y = -4},
    {.bitmap_index = 1008, .adv_w = 115, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1016, .adv_w = 104, .box_w = 6, .box_h = 16, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 1028, .adv_w = 136, .box_w = 2, .box_h = 18, .ofs_x = 3, .ofs_y = -4},
    {.bitmap_index = 1033, .adv_w = 104, .box_w = 6, .box_h = 16, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 1045, .adv_w = 143, .box_w = 8, .box_h = 3, .ofs_x = 1, .ofs_y = 5},
    {.bitmap_index = 1048, .adv_w = 180, .box_w = 9, .box_h = 15, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1065, .adv_w = 180, .box_w = 9, .box_h = 15, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1082, .adv_w = 324, .box_w = 21, .box_h = 18, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 1130, .adv_w = 252, .box_w = 15, .box_h = 9, .ofs_x = 0, .ofs_y = 2},
    {.bitmap_index = 1147, .adv_w = 252, .box_w = 15, .box_h = 9, .ofs_x = 0, .ofs_y = 2}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_1[] = {
    0x0, 0x1, 0x1e, 0x24, 0x25
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 32, .range_length = 95, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    },
    {
        .range_start = 61523, .range_length = 38, .glyph_id_start = 96,
        .unicode_list = unicode_list_1, .glyph_id_ofs_list = NULL, .list_length = 5, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};

/*-----------------
 *    KERNING
 *----------------*/


/*Map glyph_ids to kern left classes*/
static const uint8_t kern_left_class_mapping[] =
{
    0, 0, 0, 0, 0, 0, 0, 1,
    0, 2, 0, 0, 0, 3, 4, 3,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 5, 6, 7, 8, 9, 10,
    0, 0, 0, 0, 11, 12, 0, 0,
    8, 13, 8, 0, 0, 14, 15, 16,
    16, 11, 17, 11, 2, 0, 0, 0,
    18, 0, 19, 20, 21, 0, 20, 22,
    0, 19, 0, 0, 0, 0, 19, 19,
    20, 20, 0, 23, 0, 21, 0, 24,
    24, 25, 24, 0, 2, 0, 0, 0,
    0, 0, 0, 0, 0
};

/*Map glyph_ids to kern right classes*/
static const uint8_t kern_right_class_mapping[] =
{
    0, 0, 0, 1, 0, 0, 0, 2,
    1, 0, 3, 0, 0, 4, 5, 4,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    6, 0, 7, 0, 8, 0, 0, 0,
    8, 0, 0, 9, 0, 0, 0, 0,
    8, 0, 8, 0, 0, 10, 11, 12,
    12, 13, 14, 15, 0, 0, 3, 0,
    0, 0, 16, 0, 17, 17, 17, 0,
    18, 0, 0, 19, 0, 0, 20, 20,
    17, 20, 17, 20, 21, 0, 20, 22,
    22, 22, 22, 23, 0, 0, 3, 0,
    0, 0, 0, 0, 0
};

/*Kern values between classes*/
static const int8_t kern_class_values[] =
{
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -10, 0, -4, 0, -6, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 19,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 8, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, -10, 0, -14,
    -4, -12, 0, -12, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, -8, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, -16, 0, 0, 0,
    0, 0, 0, -2, 10, -10, 0, -4,
    0, -8, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, -3, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -6, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, -8, 0, 0, -2,
    0, 0, -4, 0, -1, -2, -2, -1,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    12, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 4, -14, 0, 4, -4, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, -2, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, -17, 0, 0,
    0, 0, 0, 0, -4, 0, -8, -2,
    -8, 0, -10, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, -2, 0, -27,
    0, 0, -8, 0, 0, 0, 0, 0,
    -4, 0, -2, 0, 0, 0, 0, 0,
    0, 0, 0, 0, -4, 0, -12, -8,
    4, -10, -4, 0, 4, 0, 0, 0,
    0, 0, -17, -14, -14, 0, -10, -12,
    -4, -8, 0, 0, 0, -4, 0, 0,
    -2, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, -10, 0, 4, -4,
    -2, 0, 0, 0, 0, 0, 0, 0,
    -4, -4, -2, 0, -2, -2, 0, 0,
    0, -4, 0, -12, 0, 4, -8, -4,
    0, 0, 0, 0, 0, 0, 0, -10,
    -10, -10, 0, -6, -8, 0, -4, 0,
    0, 0, 0, 0, 0, 0, 0, 21,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, -2, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, -2, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, -4, -1, 4, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 12, 0, 8, -4, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 8, 0, 0, -31, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -4, -4, -2, 0, 0, 0, 0,
    0, 8, 0, 0, -8, 0, 4, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    -2, 0, 0, 0, 0, 0, 0
};


/*Collect the kern class' data in one place*/
static const lv_font_fmt_txt_kern_classes_t kern_classes =
{
    .class_pair_values   = kern_class_values,
    .left_class_mapping  = kern_left_class_mapping,
    .right_class_mapping = kern_right_class_mapping,
    .left_class_cnt      = 25,
    .right_class_cnt     = 23,
};

/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = &kern_classes,
    .kern_scale = 16,
    .cmap_num = 2,
    .bpp = 1,
    .kern_classes = 1,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t ui_font_18 = {
#else
lv_font_t ui_font_18 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 20,          /*The maximum line height required by the font*/
    .base_line = 4,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -2,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if UI_FONT_18*/

