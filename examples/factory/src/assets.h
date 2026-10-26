#ifndef MY_ASSETS_H
#define MY_ASSETS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

// image
LV_IMG_DECLARE(img_start)


// font
LV_FONT_DECLARE(Font_Mono_Bold_14)
LV_FONT_DECLARE(Font_Mono_Bold_15)
LV_FONT_DECLARE(Font_Mono_Bold_16)
LV_FONT_DECLARE(Font_Mono_Bold_18)
LV_FONT_DECLARE(Font_Mono_Bold_20)

// Bitmap (1bpp) reader fonts converted from BDF — see src/fonts_bitmap/
LV_FONT_DECLARE(lv_font_spleen_5x8)
LV_FONT_DECLARE(lv_font_spleen_8x16)
LV_FONT_DECLARE(lv_font_spleen_12x24)
LV_FONT_DECLARE(lv_font_spleen_16x32)
LV_FONT_DECLARE(lv_font_spleen_32x64)
LV_FONT_DECLARE(lv_font_tamzen_6x12)
LV_FONT_DECLARE(lv_font_tamzen_8x16)
LV_FONT_DECLARE(lv_font_tamzen_10x20)
LV_FONT_DECLARE(lv_font_tamzen_10x20_bold)
LV_FONT_DECLARE(lv_font_terminus_12x24_bold)
LV_FONT_DECLARE(lv_font_tom_thumb)

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif