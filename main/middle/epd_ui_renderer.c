#include "epd_ui_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "epd_ui_font.h"

typedef enum {
    UI_BLACK = VP_UI_PLANE_BLACK,
    UI_RED = VP_UI_PLANE_RED,
} ui_color_t;

static void pixel(uint8_t *image, ui_color_t color, int x, int y)
{
    if (image == NULL || x < 0 || x >= VP_UI_WIDTH || y < 0 || y >= VP_UI_HEIGHT) {
        return;
    }

    /* 客户视角为 250x122 横屏，写入时转换为面板物理 RAM 坐标。 */
    int native_x = y;
    int native_y = (VP_UI_WIDTH - 1) - x;
    size_t offset = (size_t)color * VP_UI_PLANE_SIZE +
                    (size_t)native_y * VP_UI_NATIVE_STRIDE + (size_t)(native_x / 8);
    image[offset] &= (uint8_t)~(0x80U >> (native_x % 8));
}

static void hline(uint8_t *image, ui_color_t color, int x0, int x1, int y)
{
    for (int x = x0; x <= x1; ++x) {
        pixel(image, color, x, y);
    }
}

static void vline(uint8_t *image, ui_color_t color, int x, int y0, int y1)
{
    for (int y = y0; y <= y1; ++y) {
        pixel(image, color, x, y);
    }
}

static void fill_rect(uint8_t *image, ui_color_t color, int x, int y, int width, int height)
{
    for (int yy = y; yy < y + height; ++yy) {
        hline(image, color, x, x + width - 1, yy);
    }
}

static void outline_rect(uint8_t *image, ui_color_t color, int x, int y, int width, int height)
{
    hline(image, color, x, x + width - 1, y);
    hline(image, color, x, x + width - 1, y + height - 1);
    vline(image, color, x, y, y + height - 1);
    vline(image, color, x + width - 1, y, y + height - 1);
}

static void draw_text(uint8_t *image, ui_color_t color, vp_ui_font_id_t font,
                      int x, int y, const char *value)
{
    vp_ui_glyph_t glyph;
    for (const char *p = value; p != NULL && *p != '\0'; ++p) {
        if (!vp_ui_font_get_glyph(font, *p, &glyph)) {
            continue;
        }
        for (int row = 0; row < glyph.height; ++row) {
            for (int col = 0; col < glyph.width; ++col) {
                size_t offset = (size_t)row * glyph.stride + (size_t)(col / 8);
                if (glyph.bitmap != NULL &&
                    (glyph.bitmap[offset] & (uint8_t)(0x80U >> (col % 8))) != 0) {
                    pixel(image, color, x + glyph.x_offset + col, y + glyph.y_offset + row);
                }
            }
        }
        x += glyph.advance;
    }
}

static void centered_text(uint8_t *image, ui_color_t color, vp_ui_font_id_t font,
                          int center_x, int y, const char *value)
{
    draw_text(image, color, font, center_x - vp_ui_font_measure(font, value) / 2, y, value);
}

static char state_abbreviation(vp_ui_state_t state)
{
    switch (state) {
    case VP_UI_STATE_RUNNING: return 'R';
    case VP_UI_STATE_PREPARE: return 'P';
    case VP_UI_STATE_FAULT: return 'F';
    case VP_UI_STATE_SHUTDOWN: return 'O';
    default: return 'S';
    }
}

static void draw_soc(uint8_t *image, const vp_ui_snapshot_t *snapshot)
{
    if (!snapshot->bms_valid || snapshot->soc_percent > 100) {
        centered_text(image, UI_BLACK, VP_UI_FONT_METRIC, 86, 31, "--");
        return;
    }

    char value[4];
    snprintf(value, sizeof(value), "%u", snapshot->soc_percent);
    int value_width = vp_ui_font_measure(VP_UI_FONT_SOC, value);
    int percent_width = vp_ui_font_measure(VP_UI_FONT_PERCENT, "%");
    const int hundred_kerning = snapshot->soc_percent == 100 ? 20 : 0;
    value_width -= hundred_kerning;
    int total_width = value_width + 2 + percent_width;
    int x = 86 - total_width / 2;
    ui_color_t color = snapshot->soc_percent < 20 ? UI_RED : UI_BLACK;
    if (snapshot->soc_percent == 100) {
        int one_width = vp_ui_font_measure(VP_UI_FONT_SOC, "1");
        draw_text(image, color, VP_UI_FONT_SOC, x, -7, "1");
        draw_text(image, color, VP_UI_FONT_SOC, x + one_width - hundred_kerning, -7, "00");
    } else {
        draw_text(image, color, VP_UI_FONT_SOC, x, -7, value);
    }
    draw_text(image, color, VP_UI_FONT_PERCENT, x + value_width + 2, 42, "%");
}

static void draw_gear_rows(uint8_t *image, const vp_ui_snapshot_t *snapshot)
{
    static const char *labels[] = {"48V", "36V", "24V"};
    static const uint8_t gears[] = {3, 2, 1};
    static const int row_top[] = {3, 24, 45};

    hline(image, UI_BLACK, 171, 246, 23);
    hline(image, UI_BLACK, 171, 246, 44);
    hline(image, UI_BLACK, 171, 246, 65);
    vline(image, UI_BLACK, 225, 3, 65);

    for (size_t i = 0; i < 3; ++i) {
        draw_text(image, UI_BLACK, VP_UI_FONT_GEAR, 181, row_top[i] - 1, labels[i]);
        if (snapshot->gear_valid && snapshot->gear == gears[i]) {
            fill_rect(image, UI_BLACK, 231, row_top[i] + 6, 9, 9);
        }
    }
}

static void draw_soc_segments(uint8_t *image, const vp_ui_snapshot_t *snapshot)
{
    const int area_x = 3;
    const int area_width = 167;
    int active = snapshot->bms_valid && snapshot->soc_percent <= 100 ?
                 (snapshot->soc_percent + 9) / 10 : 0;
    ui_color_t fill_color = snapshot->bms_valid && snapshot->soc_percent < 20 ?
                            UI_RED : UI_BLACK;

    for (int i = 0; i < 10; ++i) {
        int x0 = area_x + (area_width * i) / 10;
        int x1 = area_x + (area_width * (i + 1)) / 10;
        if (i > 0) {
            vline(image, UI_BLACK, x0, 96, 118);
        }
        if (i < active) {
            fill_rect(image, fill_color, x0 + 2, 98, x1 - x0 - 3, 19);
        }
    }
}

static void draw_voltage(uint8_t *image, const vp_ui_snapshot_t *snapshot)
{
    char value[12];
    if (!snapshot->bms_valid || snapshot->voltage_mv < 0 || snapshot->voltage_mv > 999900) {
        memcpy(value, "--.-", 5);
    } else {
        snprintf(value, sizeof(value), "%ld.%01ld",
                 (long)(snapshot->voltage_mv / 1000),
                 labs((long)(snapshot->voltage_mv % 1000)) / 100);
    }

    int value_width = vp_ui_font_measure(VP_UI_FONT_METRIC, value);
    int unit_width = vp_ui_font_measure(VP_UI_FONT_GEAR, "V");
    int x = 209 - (value_width + 2 + unit_width) / 2;
    draw_text(image, UI_BLACK, VP_UI_FONT_METRIC, x, 64, value);
    draw_text(image, UI_BLACK, VP_UI_FONT_GEAR, x + value_width + 2, 74, "V");
}

static void draw_lightning(uint8_t *image)
{
    static const int points[][2] = {
        {210, 27}, {239, 27}, {224, 53}, {240, 53},
        {190, 95}, {203, 62}, {190, 62},
    };

    /* 黑色闪电与电池并列，避免黑红两个显存平面在同一像素重叠。 */
    for (int y = 27; y <= 95; ++y) {
        for (int x = 190; x <= 240; ++x) {
            bool inside = false;
            for (size_t i = 0, j = 6; i < 7; j = i++) {
                int xi = points[i][0];
                int yi = points[i][1];
                int xj = points[j][0];
                int yj = points[j][1];
                if ((yi > y) != (yj > y) &&
                    x < (xj - xi) * (y - yi) / (yj - yi) + xi) {
                    inside = !inside;
                }
            }
            if (inside) {
                pixel(image, UI_BLACK, x, y);
            }
        }
    }
}

static void draw_charging_page(uint8_t *image, const vp_ui_snapshot_t *snapshot)
{
    const int battery_x = 30;
    const int battery_y = 27;
    const int battery_width = 145;
    const int battery_height = 68;
    const int inner_width = battery_width - 8;

    outline_rect(image, UI_BLACK, battery_x, battery_y, battery_width, battery_height);
    fill_rect(image, UI_BLACK, battery_x + battery_width, battery_y + 22, 8, 24);

    int fill_width = (inner_width * snapshot->soc_percent) / 100;
    if (fill_width > 0) {
        fill_rect(image, UI_RED, battery_x + 4, battery_y + 4, fill_width, battery_height - 8);
    }

    draw_lightning(image);
}

static bool charging_page_active(const vp_ui_snapshot_t *snapshot)
{
    return snapshot->state != VP_UI_STATE_FAULT && snapshot->bms_valid &&
           snapshot->soc_percent < 20 && snapshot->charge_mos_active;
}

void vp_epd_ui_render(const vp_ui_snapshot_t *snapshot, uint8_t *image, size_t image_size)
{
    if (snapshot == NULL || image == NULL || image_size < VP_UI_IMAGE_SIZE) {
        return;
    }
    memset(image, 0xFF, VP_UI_IMAGE_SIZE);

    if (charging_page_active(snapshot)) {
        draw_charging_page(image, snapshot);
        return;
    }

    vline(image, UI_BLACK, 170, 3, 94);
    hline(image, UI_BLACK, 3, 246, 95);
    vline(image, UI_BLACK, 170, 96, 118);
    vline(image, UI_BLACK, 225, 96, 118);

    draw_soc(image, snapshot);
    draw_gear_rows(image, snapshot);
    draw_voltage(image, snapshot);
    draw_soc_segments(image, snapshot);

    if (snapshot->state == VP_UI_STATE_FAULT) {
        fill_rect(image, UI_RED, 172, 97, 52, 23);
    }

    char state[2] = {state_abbreviation(snapshot->state), '\0'};
    centered_text(image, UI_BLACK, VP_UI_FONT_STATE, 236, 92, state);
}

bool vp_epd_ui_fault_changed(const vp_ui_snapshot_t *old_value,
                             const vp_ui_snapshot_t *new_value)
{
    return old_value != NULL && new_value != NULL &&
           (old_value->fault_code != new_value->fault_code ||
            (old_value->state == VP_UI_STATE_FAULT) != (new_value->state == VP_UI_STATE_FAULT));
}

bool vp_epd_ui_needs_refresh(const vp_ui_snapshot_t *old_value,
                             const vp_ui_snapshot_t *new_value)
{
    if (old_value == NULL || new_value == NULL) {
        return true;
    }
    return old_value->bms_valid != new_value->bms_valid ||
           old_value->gear_valid != new_value->gear_valid ||
           old_value->soc_percent != new_value->soc_percent ||
           old_value->gear != new_value->gear || old_value->state != new_value->state ||
           old_value->fault_code != new_value->fault_code ||
           old_value->charge_mos_active != new_value->charge_mos_active ||
           labs((long)new_value->voltage_mv - old_value->voltage_mv) >= 500;
}
