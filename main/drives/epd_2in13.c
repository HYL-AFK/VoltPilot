#include "epd_2in13.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "vp_board.h"
#include "vp_types.h"

static const char *TAG = "epd_2in13";

#if !VP_EPD_USE_SOFTWARE_SPI
static spi_device_handle_t s_epd_spi;
#endif

static uint8_t s_4color_compat[VP_EPD_BUFFER_SIZE];

static void epd_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static esp_err_t epd_write_byte(uint8_t value)
{
#if VP_EPD_USE_SOFTWARE_SPI
    gpio_set_level(VP_EPD_PIN_CS, 0);

    for (int i = 0; i < 8; i++) {
        gpio_set_level(VP_EPD_PIN_SCK, 0);
        gpio_set_level(VP_EPD_PIN_MOSI, (value & 0x80) != 0);
        esp_rom_delay_us(VP_EPD_SOFTWARE_SPI_DELAY_US);
        gpio_set_level(VP_EPD_PIN_SCK, 1);
        esp_rom_delay_us(VP_EPD_SOFTWARE_SPI_DELAY_US);
        value <<= 1;
    }

    gpio_set_level(VP_EPD_PIN_CS, 1);
    return ESP_OK;
#else
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &value,
    };

    return spi_device_polling_transmit(s_epd_spi, &t);
#endif
}

static esp_err_t epd_send_command(uint8_t command)
{
    gpio_set_level(VP_EPD_PIN_DC, 0);
    esp_err_t err = epd_write_byte(command);
    gpio_set_level(VP_EPD_PIN_DC, 1);
    return err;
}

static esp_err_t epd_send_data_byte(uint8_t data)
{
    gpio_set_level(VP_EPD_PIN_DC, 1);
    return epd_write_byte(data);
}

static esp_err_t epd_send_data(const uint8_t *data, size_t len)
{
    gpio_set_level(VP_EPD_PIN_DC, 1);

    for (size_t i = 0; i < len; i++) {
        ESP_RETURN_ON_ERROR(epd_write_byte(data[i]), TAG, "send data byte failed");
    }

    return ESP_OK;
}

static esp_err_t epd_send_command_data(uint8_t command, const uint8_t *data, size_t len, const char *phase)
{
    ESP_RETURN_ON_ERROR(epd_send_command(command), TAG, "%s cmd failed", phase);

    for (size_t i = 0; i < len; i++) {
        ESP_RETURN_ON_ERROR(epd_send_data_byte(data[i]), TAG, "%s data failed", phase);
    }

    ESP_LOGI(TAG, "init %s cmd=0x%02X len=%u", phase, command, (unsigned)len);
    return ESP_OK;
}

static esp_err_t epd_wait_ready_high(const char *phase)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(VP_EPD_BUSY_TIMEOUT_MS);
    int first_level = gpio_get_level(VP_EPD_PIN_BUSY);

    while (gpio_get_level(VP_EPD_PIN_BUSY) == 0) {
        if ((xTaskGetTickCount() - start) >= timeout) {
            ESP_LOGE(TAG, "%s BUSY ready high timeout, first=%d level=%d",
                     phase, first_level, gpio_get_level(VP_EPD_PIN_BUSY));
            return ESP_ERR_TIMEOUT;
        }
        epd_delay_ms(10);
    }

    uint32_t elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - start);
    ESP_LOGI(TAG, "%s BUSY ready high first=%d idle=1 elapsed=%" PRIu32 " ms",
             phase, first_level, elapsed_ms);
    return ESP_OK;
}

static uint8_t epd_packed_color(uint8_t color)
{
    color &= 0x03;
    return (uint8_t)((color << 6) | (color << 4) | (color << 2) | color);
}

static void epd_set_4color_pixel(uint8_t *image, int x, int y, uint8_t color)
{
    if (image == NULL || x < 0 || x >= VP_EPD_WIDTH || y < 0 || y >= VP_EPD_HEIGHT) {
        return;
    }

    size_t offset = (size_t)y * VP_EPD_WIDTH_BYTES + (x / VP_EPD_PIXELS_PER_BYTE);
    uint8_t shift = (uint8_t)(6 - (2 * (x % VP_EPD_PIXELS_PER_BYTE)));
    image[offset] &= (uint8_t)~(0x03 << shift);
    image[offset] |= (uint8_t)((color & 0x03) << shift);
}

static esp_err_t epd_reset(void)
{
    epd_delay_ms(100);
    gpio_set_level(VP_EPD_PIN_RST, 0);
    epd_delay_ms(10);
    gpio_set_level(VP_EPD_PIN_RST, 1);
    epd_delay_ms(10);

    return epd_wait_ready_high("hardware reset");
}

static esp_err_t epd_vendor_init_sequence(void)
{
    static const uint8_t cmd_4d[] = {0x78};
    static const uint8_t cmd_00[] = {0x0F, 0x09};
    static const uint8_t cmd_01[] = {0x07, 0x00, 0x22, 0x78, 0x0A, 0x22};
    static const uint8_t cmd_03[] = {0x10, 0x54, 0x44};
    static const uint8_t cmd_06[] = {0x0F, 0x0A, 0x2F, 0x25, 0x22, 0x2E, 0x21};
    static const uint8_t cmd_30[] = {0x02};
    static const uint8_t cmd_41[] = {0x00};
    static const uint8_t cmd_50[] = {0x37};
    static const uint8_t cmd_60[] = {0x02, 0x02};
    static const uint8_t cmd_61[] = {
        VP_EPD_SOURCE_BITS / 256,
        VP_EPD_SOURCE_BITS % 256,
        VP_EPD_GATE_BITS / 256,
        VP_EPD_GATE_BITS % 256,
    };
    static const uint8_t cmd_65[] = {0x00, 0x00, 0x00, 0x00};
    static const uint8_t cmd_e7[] = {0x1C};
    static const uint8_t cmd_e3[] = {0x22};
    static const uint8_t cmd_e0[] = {0x00};
    static const uint8_t cmd_b4[] = {0xD0};
    static const uint8_t cmd_b5[] = {0x03};
    static const uint8_t cmd_e9[] = {0x01};

    ESP_RETURN_ON_ERROR(epd_send_command_data(0x4D, cmd_4d, sizeof(cmd_4d), "0x4D"), TAG, "0x4D failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0x00, cmd_00, sizeof(cmd_00), "0x00 panel setting"), TAG, "0x00 failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0x01, cmd_01, sizeof(cmd_01), "0x01 power setting"), TAG, "0x01 failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0x03, cmd_03, sizeof(cmd_03), "0x03 power off seq"), TAG, "0x03 failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0x06, cmd_06, sizeof(cmd_06), "0x06 booster"), TAG, "0x06 failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0x30, cmd_30, sizeof(cmd_30), "0x30 frame rate"), TAG, "0x30 failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0x41, cmd_41, sizeof(cmd_41), "0x41"), TAG, "0x41 failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0x50, cmd_50, sizeof(cmd_50), "0x50 vcom/data interval"), TAG, "0x50 failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0x60, cmd_60, sizeof(cmd_60), "0x60 tcon"), TAG, "0x60 failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0x61, cmd_61, sizeof(cmd_61), "0x61 resolution"), TAG, "0x61 failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0x65, cmd_65, sizeof(cmd_65), "0x65"), TAG, "0x65 failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0xE7, cmd_e7, sizeof(cmd_e7), "0xE7"), TAG, "0xE7 failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0xE3, cmd_e3, sizeof(cmd_e3), "0xE3"), TAG, "0xE3 failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0xE0, cmd_e0, sizeof(cmd_e0), "0xE0"), TAG, "0xE0 failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0xB4, cmd_b4, sizeof(cmd_b4), "0xB4"), TAG, "0xB4 failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0xB5, cmd_b5, sizeof(cmd_b5), "0xB5"), TAG, "0xB5 failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0xE9, cmd_e9, sizeof(cmd_e9), "0xE9"), TAG, "0xE9 failed");
    return ESP_OK;
}

static esp_err_t epd_refresh(void)
{
    ESP_LOGI(TAG, "refresh 0x04/0x12 vendor flow");
    ESP_RETURN_ON_ERROR(epd_send_command(0x04), TAG, "power on cmd failed");
    ESP_RETURN_ON_ERROR(epd_wait_ready_high("refresh power on 0x04"), TAG, "power on busy failed");

    ESP_RETURN_ON_ERROR(epd_send_command(0x12), TAG, "display refresh cmd failed");
    ESP_RETURN_ON_ERROR(epd_send_data_byte(0x00), TAG, "display refresh data failed");
    ESP_RETURN_ON_ERROR(epd_wait_ready_high("refresh display 0x12"), TAG, "refresh busy failed");
    return ESP_OK;
}

esp_err_t epd_2in13_init(void)
{
#if !VP_EPD_USE_SOFTWARE_SPI
    if (s_epd_spi == NULL) {
        spi_bus_config_t buscfg = {
            .mosi_io_num = VP_EPD_PIN_MOSI,
            .miso_io_num = -1,
            .sclk_io_num = VP_EPD_PIN_SCK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = VP_EPD_BUFFER_SIZE,
        };
        spi_device_interface_config_t devcfg = {
            .clock_speed_hz = VP_EPD_SPI_CLOCK_HZ,
            .mode = 0,
            .spics_io_num = VP_EPD_PIN_CS,
            .queue_size = 1,
        };

        ESP_RETURN_ON_ERROR(spi_bus_initialize(VP_EPD_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi bus init failed");
        ESP_RETURN_ON_ERROR(spi_bus_add_device(VP_EPD_HOST, &devcfg, &s_epd_spi), TAG, "spi add device failed");
    }
#endif

    gpio_config_t output_cfg = {
        .pin_bit_mask = (1ULL << VP_EPD_PIN_DC) | (1ULL << VP_EPD_PIN_RST)
#if VP_EPD_USE_SOFTWARE_SPI
                        | (1ULL << VP_EPD_PIN_SCK) | (1ULL << VP_EPD_PIN_MOSI) | (1ULL << VP_EPD_PIN_CS)
#endif
                        ,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output_cfg), TAG, "gpio output config failed");

#if VP_EPD_USE_SOFTWARE_SPI
    gpio_set_level(VP_EPD_PIN_CS, 1);
    gpio_set_level(VP_EPD_PIN_SCK, 1);
    gpio_set_level(VP_EPD_PIN_MOSI, 1);
    gpio_set_level(VP_EPD_PIN_DC, 1);
#endif

    gpio_config_t input_cfg = {
        .pin_bit_mask = (1ULL << VP_EPD_PIN_BUSY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&input_cfg), TAG, "gpio input config failed");

    ESP_LOGI(TAG,
             "EPD panel=%s controller=%s 4SPI BWRY colors=4 buffer=%u bytes source=%d gate=%d pins SCK=%d MOSI=%d CS=%d DC=%d RST=%d BUSY=%d spi=%d Hz supply=%d mV",
             VP_EPD_PANEL_MODEL_BWRY, VP_EPD_CONTROLLER_NAME, (unsigned)VP_EPD_BUFFER_SIZE,
             VP_EPD_SOURCE_BITS, VP_EPD_GATE_BITS, VP_EPD_PIN_SCK, VP_EPD_PIN_MOSI, VP_EPD_PIN_CS,
             VP_EPD_PIN_DC, VP_EPD_PIN_RST, VP_EPD_PIN_BUSY, VP_EPD_SPI_CLOCK_HZ,
             VP_EPD_MODULE_SUPPLY_MV);

    ESP_RETURN_ON_ERROR(epd_reset(), TAG, "reset failed");
    ESP_RETURN_ON_ERROR(epd_vendor_init_sequence(), TAG, "vendor init failed");

    ESP_LOGI(TAG, "EPD initialized");
    return ESP_OK;
}

esp_err_t epd_2in13_clear(void)
{
    memset(s_4color_compat, epd_packed_color(VP_EPD_COLOR_WHITE), sizeof(s_4color_compat));
    return epd_2in13_display_4color(s_4color_compat);
}

esp_err_t epd_2in13_display_4color(const uint8_t *image)
{
    ESP_RETURN_ON_FALSE(image != NULL, ESP_ERR_INVALID_ARG, TAG, "4-color image buffer is null");

    ESP_RETURN_ON_ERROR(epd_send_command(0x10), TAG, "4-color ram cmd failed");
    ESP_LOGI(TAG, "write RAM 0x10 bytes=%u", (unsigned)VP_EPD_BUFFER_SIZE);
    ESP_RETURN_ON_ERROR(epd_send_data(image, VP_EPD_BUFFER_SIZE), TAG, "4-color ram data failed");
    ESP_RETURN_ON_ERROR(epd_refresh(), TAG, "4-color refresh failed");
    ESP_LOGI(TAG, "EPD refreshed 4-color image");
    return ESP_OK;
}

esp_err_t epd_2in13_display_bw(const uint8_t *black, const uint8_t *red)
{
    ESP_RETURN_ON_FALSE(black != NULL, ESP_ERR_INVALID_ARG, TAG, "black buffer is null");

    int old_stride = (VP_EPD_WIDTH + 7) / 8;
    memset(s_4color_compat, epd_packed_color(VP_EPD_COLOR_WHITE), sizeof(s_4color_compat));

    for (int y = 0; y < VP_EPD_HEIGHT; y++) {
        for (int x = 0; x < VP_EPD_WIDTH; x++) {
            uint8_t mask = (uint8_t)(0x80 >> (x % 8));
            bool is_black = (black[y * old_stride + (x / 8)] & mask) == 0;
            bool is_red = red != NULL && ((red[y * old_stride + (x / 8)] & mask) == 0);
            uint8_t color = is_red ? VP_EPD_COLOR_RED : (is_black ? VP_EPD_COLOR_BLACK : VP_EPD_COLOR_WHITE);
            epd_set_4color_pixel(s_4color_compat, x, y, color);
        }
    }

    return epd_2in13_display_4color(s_4color_compat);
}

esp_err_t epd_2in13_sleep(void)
{
    ESP_RETURN_ON_ERROR(epd_send_command(0x02), TAG, "power off cmd failed");
    ESP_RETURN_ON_ERROR(epd_send_data_byte(0x00), TAG, "power off data failed");
    ESP_RETURN_ON_ERROR(epd_wait_ready_high("power off"), TAG, "power off busy failed");
    ESP_RETURN_ON_ERROR(epd_send_command(0x07), TAG, "deep sleep cmd failed");
    ESP_RETURN_ON_ERROR(epd_send_data_byte(0xA5), TAG, "deep sleep data failed");
    epd_delay_ms(100);

    ESP_LOGI(TAG, "EPD sleep");
    return ESP_OK;
}
