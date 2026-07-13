#include "display_service.h"

#include <string.h>

#include "esp_log.h"

#include "epd_2in13.h"
#include "vp_board.h"
#include "vp_types.h"

static const char *TAG = "display_service";

static uint8_t s_image[VP_EPD_PLANE_SIZE];

_Static_assert(sizeof(s_image) == 8000, "four-color EPD image must be 8000 bytes");

static void fill_screen(uint8_t color)
{
    memset(s_image, (uint8_t)(color * 0x55U), sizeof(s_image));
}

static void draw_pixel(uint8_t color, int x, int y)
{
    if (x < 0 || x >= VP_EPD_WIDTH || y < 0 || y >= VP_EPD_HEIGHT) {
        return;
    }

    size_t offset = (size_t)y * VP_EPD_STRIDE_BYTES + (size_t)(x / 4);
    uint8_t shift = (uint8_t)(6 - 2 * (x % 4));
    uint8_t mask = (uint8_t)(0x03U << shift);
    s_image[offset] = (uint8_t)((s_image[offset] & (uint8_t)~mask) |
                                ((color & 0x03U) << shift));
}

static void draw_rect(uint8_t color, int x, int y, int width, int height)
{
    for (int yy = y; yy < y + height; yy++) {
        for (int xx = x; xx < x + width; xx++) {
            draw_pixel(color, xx, yy);
        }
    }
}

static void draw_boot_pattern(void)
{
#if VP_EPD_BOOT_FULL_BLACK_TEST
    ESP_LOGW(TAG, "EPD boot full-black test enabled");
    fill_screen(VP_EPD_COLOR_BLACK);
#else
    int band_height = VP_EPD_HEIGHT / 4;

    fill_screen(VP_EPD_COLOR_WHITE);
    draw_rect(VP_EPD_COLOR_BLACK, 0, 0, VP_EPD_WIDTH, band_height);
    draw_rect(VP_EPD_COLOR_WHITE, 0, band_height, VP_EPD_WIDTH, band_height);
    draw_rect(VP_EPD_COLOR_YELLOW, 0, band_height * 2,
              VP_EPD_WIDTH, band_height);
    draw_rect(VP_EPD_COLOR_RED, 0, band_height * 3,
              VP_EPD_WIDTH, VP_EPD_HEIGHT - (band_height * 3));

    for (int x = 0; x < VP_EPD_WIDTH; x++) {
        draw_pixel(VP_EPD_COLOR_BLACK, x, band_height);
        draw_pixel(VP_EPD_COLOR_BLACK, x, (band_height * 3) - 1);
    }

    ESP_LOGI(TAG, "EPD boot four-color test pattern: black/white/yellow/red horizontal bands");
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
    return epd_2in13_display(s_image);
}
