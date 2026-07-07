#include "display_service.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

#include "epd_2in13.h"
#include "vp_types.h"

static const char *TAG = "display_service";

static uint8_t s_black[VP_EPD_BUFFER_SIZE];
static uint8_t s_red[VP_EPD_BUFFER_SIZE];

static void draw_pixel(uint8_t *buf, int x, int y, bool black)
{
    if (x < 0 || x >= VP_EPD_WIDTH || y < 0 || y >= VP_EPD_HEIGHT) {
        return;
    }

    uint8_t mask = 0x80 >> (x % 8);
    uint8_t *byte = &buf[(y * VP_EPD_WIDTH_BYTES) + (x / 8)];

    if (black) {
        *byte &= (uint8_t)~mask;
    } else {
        *byte |= mask;
    }
}

static void draw_rect(uint8_t *buf, int x, int y, int w, int h, bool black)
{
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            draw_pixel(buf, xx, yy, black);
        }
    }
}

static void draw_hline(uint8_t *buf, int x, int y, int w)
{
    draw_rect(buf, x, y, w, 1, true);
}

static void draw_vline(uint8_t *buf, int x, int y, int h)
{
    draw_rect(buf, x, y, 1, h, true);
}

static void draw_frame(uint8_t *buf)
{
    draw_hline(buf, 0, 0, VP_EPD_WIDTH);
    draw_hline(buf, 0, VP_EPD_HEIGHT - 1, VP_EPD_WIDTH);
    draw_vline(buf, 0, 0, VP_EPD_HEIGHT);
    draw_vline(buf, VP_EPD_WIDTH - 1, 0, VP_EPD_HEIGHT);

    draw_hline(buf, 8, 38, VP_EPD_WIDTH - 16);
    draw_hline(buf, 8, 202, VP_EPD_WIDTH - 16);
    draw_vline(buf, 24, 48, 138);
    draw_vline(buf, 61, 48, 138);
    draw_vline(buf, 98, 48, 138);
}

static void draw_block_letter(uint8_t *buf, char c, int x, int y, int scale)
{
    static const uint8_t glyphs[][8] = {
        {'V', 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04, 0x04},
        {'O', 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
        {'L', 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
        {'T', 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
        {'P', 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
        {'I', 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F},
    };

    const uint8_t *rows = NULL;
    for (size_t i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); i++) {
        if (glyphs[i][0] == (uint8_t)c) {
            rows = &glyphs[i][1];
            break;
        }
    }

    if (rows == NULL) {
        return;
    }

    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            if ((rows[row] & (0x10 >> col)) != 0) {
                draw_rect(buf, x + (col * scale), y + (row * scale), scale, scale, true);
            }
        }
    }
}

static void draw_word_volt_pilot(uint8_t *buf)
{
    const char *line1 = "VOLT";
    const char *line2 = "PILOT";
    int scale = 3;
    int step = 6 * scale;
    int x1 = 25;
    int x2 = 16;
    int y1 = 76;
    int y2 = 126;

    for (int i = 0; line1[i] != '\0'; i++) {
        draw_block_letter(buf, line1[i], x1 + (i * step), y1, scale);
    }
    for (int i = 0; line2[i] != '\0'; i++) {
        draw_block_letter(buf, line2[i], x2 + (i * step), y2, scale);
    }
}

static void draw_boot_pattern(void)
{
    memset(s_black, 0xFF, sizeof(s_black));
    memset(s_red, 0xFF, sizeof(s_red));

    draw_frame(s_black);
    draw_word_volt_pilot(s_black);

    draw_rect(s_black, 10, 214, 102, 6, true);
    draw_rect(s_black, 10, 226, 72, 6, true);
    draw_rect(s_black, 10, 238, 42, 6, true);
}

esp_err_t display_service_init(void)
{
    esp_err_t err = epd_2in13_init();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "display service initialized");
    }
    return err;
}

esp_err_t display_service_clear(void)
{
    return epd_2in13_clear();
}

esp_err_t display_service_show_boot_screen(void)
{
    draw_boot_pattern();
    return epd_2in13_display_bw(s_black, s_red);
}
