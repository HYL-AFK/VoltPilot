#ifndef EPD_UI_FONT_H
#define EPD_UI_FONT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    VP_UI_FONT_METRIC = 0,
    VP_UI_FONT_SOC,
    VP_UI_FONT_PERCENT,
    VP_UI_FONT_GEAR,
    VP_UI_FONT_STATE,
} vp_ui_font_id_t;

typedef struct {
    uint8_t width;
    uint8_t height;
    int8_t x_offset;
    int8_t y_offset;
    uint8_t advance;
    uint8_t stride;
    const uint8_t *bitmap;
} vp_ui_glyph_t;

bool vp_ui_font_get_glyph(vp_ui_font_id_t font, char character, vp_ui_glyph_t *out_glyph);
int vp_ui_font_measure(vp_ui_font_id_t font, const char *text);

#endif
