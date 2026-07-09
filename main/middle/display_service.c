#include "display_service.h"

#include <string.h>

#include "esp_log.h"

#include "epd_2in13.h"
#include "vp_board.h"
#include "vp_types.h"

static const char *TAG = "display_service";

static uint8_t s_epd_image[VP_EPD_BUFFER_SIZE];

static uint8_t pack_color(uint8_t color)
{
    color &= 0x03;
    return (uint8_t)((color << 6) | (color << 4) | (color << 2) | color);
}

static void fill_screen(uint8_t color)
{
    memset(s_epd_image, pack_color(color), sizeof(s_epd_image));
}

static void draw_pixel(uint8_t color, int x, int y)
{
    if (x < 0 || x >= VP_EPD_WIDTH || y < 0 || y >= VP_EPD_HEIGHT) {
        return;
    }

    size_t offset = (size_t)y * VP_EPD_WIDTH_BYTES + (x / VP_EPD_PIXELS_PER_BYTE);
    uint8_t shift = (uint8_t)(6 - (2 * (x % VP_EPD_PIXELS_PER_BYTE)));
    s_epd_image[offset] &= (uint8_t)~(0x03 << shift);
    s_epd_image[offset] |= (uint8_t)((color & 0x03) << shift);
}

static void draw_rect(uint8_t color, int x, int y, int w, int h)
{
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            draw_pixel(color, xx, yy);
        }
    }
}

static void draw_boot_pattern(void)
{
#if VP_EPD_BOOT_FULL_BLACK_TEST
    ESP_LOGW(TAG, "EPD boot full black test enabled");
    fill_screen(VP_EPD_COLOR_BLACK);
#else
    int half_w = VP_EPD_WIDTH / 2;
    int half_h = VP_EPD_HEIGHT / 2;

    fill_screen(VP_EPD_COLOR_WHITE);
    draw_rect(VP_EPD_COLOR_BLACK, 0, 0, half_w, half_h);
    draw_rect(VP_EPD_COLOR_WHITE, half_w, 0, VP_EPD_WIDTH - half_w, half_h);
    draw_rect(VP_EPD_COLOR_YELLOW, 0, half_h, half_w, VP_EPD_HEIGHT - half_h);
    draw_rect(VP_EPD_COLOR_RED, half_w, half_h, VP_EPD_WIDTH - half_w, VP_EPD_HEIGHT - half_h);

    for (int x = 0; x < VP_EPD_WIDTH; x++) {
        draw_pixel(VP_EPD_COLOR_BLACK, x, half_h - 1);
        draw_pixel(VP_EPD_COLOR_BLACK, x, half_h);
    }
    for (int y = 0; y < VP_EPD_HEIGHT; y++) {
        draw_pixel(VP_EPD_COLOR_BLACK, half_w - 1, y);
        draw_pixel(VP_EPD_COLOR_BLACK, half_w, y);
    }

    ESP_LOGI(TAG, "EPD boot 4-color test pattern: black/white/yellow/red");
#endif
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
    return epd_2in13_display_4color(s_epd_image);
}
