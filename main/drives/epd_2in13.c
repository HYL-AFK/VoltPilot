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

_Static_assert(VP_EPD_STRIDE_BYTES == 16, "SSD1680 stride must be 16 bytes");
_Static_assert(VP_EPD_PLANE_SIZE == 4000, "SSD1680 plane must be 4000 bytes");

#if !VP_EPD_USE_SOFTWARE_SPI
static spi_device_handle_t s_epd_spi;
#endif

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
    spi_transaction_t transaction = {
        .length = 8,
        .tx_buffer = &value,
    };

    return spi_device_polling_transmit(s_epd_spi, &transaction);
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

static esp_err_t epd_send_data(const uint8_t *data, size_t len, bool invert)
{
    gpio_set_level(VP_EPD_PIN_DC, 1);

    for (size_t i = 0; i < len; i++) {
        uint8_t value = invert ? (uint8_t)~data[i] : data[i];
        ESP_RETURN_ON_ERROR(epd_write_byte(value), TAG, "send data byte failed");
    }

    return ESP_OK;
}

static esp_err_t epd_send_fill(uint8_t value, size_t len, bool invert)
{
    uint8_t wire_value = invert ? (uint8_t)~value : value;

    gpio_set_level(VP_EPD_PIN_DC, 1);
    for (size_t i = 0; i < len; i++) {
        ESP_RETURN_ON_ERROR(epd_write_byte(wire_value), TAG, "send fill byte failed");
    }

    return ESP_OK;
}

static esp_err_t epd_send_command_data(uint8_t command, const uint8_t *data, size_t len, const char *phase)
{
    ESP_RETURN_ON_ERROR(epd_send_command(command), TAG, "%s command failed", phase);
    for (size_t i = 0; i < len; i++) {
        ESP_RETURN_ON_ERROR(epd_send_data_byte(data[i]), TAG, "%s data failed", phase);
    }

    ESP_LOGI(TAG, "send %s cmd=0x%02X len=%u", phase, (unsigned)command, (unsigned)len);
    return ESP_OK;
}

static esp_err_t epd_wait_idle(const char *phase)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(VP_EPD_BUSY_TIMEOUT_MS);
    int first_level = gpio_get_level(VP_EPD_PIN_BUSY);

    while (gpio_get_level(VP_EPD_PIN_BUSY) == VP_EPD_BUSY_ACTIVE_LEVEL) {
        if ((xTaskGetTickCount() - start) >= timeout) {
            ESP_LOGE(TAG, "%s BUSY stuck high, first=%d level=%d",
                     phase, first_level, gpio_get_level(VP_EPD_PIN_BUSY));
            return ESP_ERR_TIMEOUT;
        }
        epd_delay_ms(10);
    }

    uint32_t elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - start);
    ESP_LOGI(TAG, "%s BUSY idle low first=%d elapsed=%" PRIu32 " ms",
             phase, first_level, elapsed_ms);
    return ESP_OK;
}

static esp_err_t epd_wait_refresh_cycle(void)
{
    TickType_t trigger = xTaskGetTickCount();
    TickType_t start_timeout = pdMS_TO_TICKS(VP_EPD_BUSY_START_TIMEOUT_MS);
    int first_level = gpio_get_level(VP_EPD_PIN_BUSY);

    while (gpio_get_level(VP_EPD_PIN_BUSY) != VP_EPD_BUSY_ACTIVE_LEVEL) {
        if ((xTaskGetTickCount() - trigger) >= start_timeout) {
            ESP_LOGE(TAG,
                     "refresh BUSY never went high, first=%d level=%d; check SPI/DC/CS and R2 4-wire selection",
                     first_level, gpio_get_level(VP_EPD_PIN_BUSY));
            return ESP_ERR_TIMEOUT;
        }
        epd_delay_ms(10);
    }

    uint32_t asserted_ms = pdTICKS_TO_MS(xTaskGetTickCount() - trigger);
    ESP_LOGI(TAG, "refresh BUSY asserted high after %" PRIu32 " ms", asserted_ms);
    ESP_RETURN_ON_ERROR(epd_wait_idle("refresh completion"), TAG, "refresh BUSY completion failed");

    uint32_t total_ms = pdTICKS_TO_MS(xTaskGetTickCount() - trigger);
    ESP_LOGI(TAG, "refresh BUSY high-to-low complete total=%" PRIu32 " ms", total_ms);
    return ESP_OK;
}

static esp_err_t epd_set_ram_cursor(void)
{
    ESP_RETURN_ON_ERROR(epd_send_command_data(0x4E, (const uint8_t[]){0x00}, 1, "set RAM X"),
                        TAG, "set RAM X failed");
    return epd_send_command_data(0x4F, (const uint8_t[]){0x27, 0x01}, 2, "set RAM Y");
}

static esp_err_t epd_reset(void)
{
    gpio_set_level(VP_EPD_PIN_RST, 0);
    epd_delay_ms(20);
    gpio_set_level(VP_EPD_PIN_RST, 1);
    epd_delay_ms(20);
    return epd_wait_idle("hardware reset");
}

static esp_err_t epd_vendor_init_sequence(void)
{
    ESP_RETURN_ON_ERROR(epd_send_command(0x12), TAG, "SSD1680 software reset failed");
    ESP_RETURN_ON_ERROR(epd_wait_idle("software reset"), TAG, "software reset wait failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0x01, (const uint8_t[]){0x27, 0x01, 0x01}, 3,
                                               "driver output control"), TAG, "driver output failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0x11, (const uint8_t[]){0x01}, 1,
                                               "data entry mode"), TAG, "data entry failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0x44, (const uint8_t[]){0x00, 0x0F}, 2,
                                               "RAM X range"), TAG, "RAM X range failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0x45, (const uint8_t[]){0x27, 0x01, 0x00, 0x00}, 4,
                                               "RAM Y range"), TAG, "RAM Y range failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0x3C, (const uint8_t[]){0x05}, 1,
                                               "border waveform"), TAG, "border waveform failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0x21, (const uint8_t[]){0x00, 0x80}, 2,
                                               "display update control"), TAG, "display update control failed");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0x18, (const uint8_t[]){0x80}, 1,
                                               "temperature sensor"), TAG, "temperature sensor failed");
    return epd_set_ram_cursor();
}

static esp_err_t epd_refresh(void)
{
    ESP_LOGI(TAG, "refresh SSD1680: update control 0x22=0xF7, activate 0x20");
    ESP_RETURN_ON_ERROR(epd_send_command_data(0x22, (const uint8_t[]){0xF7}, 1,
                                               "display update control"), TAG, "update control failed");
    ESP_RETURN_ON_ERROR(epd_send_command(0x20), TAG, "display update activate failed");
    return epd_wait_idle("display refresh");
}

static esp_err_t epd_write_plane(uint8_t command, const uint8_t *plane, bool invert, const char *name)
{
    ESP_RETURN_ON_ERROR(epd_set_ram_cursor(), TAG, "set cursor for %s plane failed", name);
    ESP_RETURN_ON_ERROR(epd_send_command(command), TAG, "%s RAM command failed", name);
    ESP_LOGI(TAG, "write %s RAM 0x%02X bytes=%u invert=%d",
             name, (unsigned)command, (unsigned)VP_EPD_PLANE_SIZE, invert);
    return epd_send_data(plane, VP_EPD_PLANE_SIZE, invert);
}

static esp_err_t epd_fill_plane(uint8_t command, uint8_t value, bool invert, const char *name)
{
    ESP_RETURN_ON_ERROR(epd_set_ram_cursor(), TAG, "set cursor for %s plane failed", name);
    ESP_RETURN_ON_ERROR(epd_send_command(command), TAG, "%s RAM command failed", name);
    uint8_t wire_value = invert ? (uint8_t)~value : value;
    ESP_LOGI(TAG, "fill %s RAM 0x%02X bytes=%u wire=0x%02X",
             name, (unsigned)command, (unsigned)VP_EPD_PLANE_SIZE, (unsigned)wire_value);
    return epd_send_fill(value, VP_EPD_PLANE_SIZE, invert);
}

esp_err_t epd_2in13_init(void)
{
#if !VP_EPD_USE_SOFTWARE_SPI
    if (s_epd_spi == NULL) {
        spi_bus_config_t bus_config = {
            .mosi_io_num = VP_EPD_PIN_MOSI,
            .miso_io_num = -1,
            .sclk_io_num = VP_EPD_PIN_SCK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = VP_EPD_PLANE_SIZE,
        };
        spi_device_interface_config_t device_config = {
            .clock_speed_hz = VP_EPD_SPI_CLOCK_HZ,
            .mode = 3,
            .spics_io_num = VP_EPD_PIN_CS,
            .queue_size = 1,
        };

        ESP_RETURN_ON_ERROR(spi_bus_initialize(VP_EPD_HOST, &bus_config, SPI_DMA_CH_AUTO),
                            TAG, "SPI bus init failed");
        ESP_RETURN_ON_ERROR(spi_bus_add_device(VP_EPD_HOST, &device_config, &s_epd_spi),
                            TAG, "SPI device add failed");
    }
#endif

    gpio_config_t output_config = {
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
    ESP_RETURN_ON_ERROR(gpio_config(&output_config), TAG, "GPIO output config failed");

#if VP_EPD_USE_SOFTWARE_SPI
    gpio_set_level(VP_EPD_PIN_CS, 1);
    gpio_set_level(VP_EPD_PIN_SCK, 1);
    gpio_set_level(VP_EPD_PIN_MOSI, 1);
#endif
    gpio_set_level(VP_EPD_PIN_DC, 1);
    gpio_set_level(VP_EPD_PIN_RST, 1);

    gpio_config_t input_config = {
        .pin_bit_mask = (1ULL << VP_EPD_PIN_BUSY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&input_config), TAG, "GPIO input config failed");

#if VP_EPD_USE_SOFTWARE_SPI
    const unsigned transport_hz = 1000000U / (2U * VP_EPD_SOFTWARE_SPI_DELAY_US);
    const char *transport = "software SPI mode 3 equivalent";
#else
    const unsigned transport_hz = VP_EPD_SPI_CLOCK_HZ;
    const char *transport = "hardware SPI mode 3";
#endif

    ESP_LOGI(TAG,
             "EPD module=%s panel=%s controller=%s black/red planes image_bytes=%u RAM=%dx%d active=%dx%d",
             VP_EPD_MODULE_MODEL, VP_EPD_PANEL_MODEL, VP_EPD_CONTROLLER_NAME,
             (unsigned)VP_EPD_PLANE_SIZE, VP_EPD_RAM_WIDTH, VP_EPD_DRIVER_GATE_COUNT,
             VP_EPD_WIDTH, VP_EPD_HEIGHT);
    ESP_LOGI(TAG,
             "EPD pins SCK=%d MOSI=%d CS=%d DC=%d RST=%d BUSY=%d busy_active=%d transport=%s approx_hz=%u",
             VP_EPD_PIN_SCK, VP_EPD_PIN_MOSI, VP_EPD_PIN_CS, VP_EPD_PIN_DC,
             VP_EPD_PIN_RST, VP_EPD_PIN_BUSY, VP_EPD_BUSY_ACTIVE_LEVEL, transport, transport_hz);

    ESP_RETURN_ON_ERROR(epd_reset(), TAG, "hardware reset failed");
    ESP_RETURN_ON_ERROR(epd_vendor_init_sequence(), TAG, "SSD1680 init failed");

    ESP_LOGI(TAG, "SSD1680 black/white/red EPD initialized");
    return ESP_OK;
}

esp_err_t epd_2in13_clear(void)
{
    ESP_RETURN_ON_ERROR(epd_wait_idle("before clear"), TAG, "BUSY before clear failed");
    ESP_RETURN_ON_ERROR(epd_fill_plane(0x24, 0xFF, false, "black clear"), TAG, "black clear failed");
    ESP_RETURN_ON_ERROR(epd_fill_plane(0x26, 0xFF, true, "red clear"), TAG, "red clear failed");
    ESP_RETURN_ON_ERROR(epd_refresh(), TAG, "clear refresh failed");
    ESP_LOGI(TAG, "EPD cleared to white");
    return ESP_OK;
}

esp_err_t epd_2in13_display(const uint8_t *image)
{
    ESP_RETURN_ON_FALSE(image != NULL, ESP_ERR_INVALID_ARG, TAG, "SSD1680 image is null");

    ESP_RETURN_ON_ERROR(epd_wait_idle("before display"), TAG, "BUSY before display failed");
    ESP_RETURN_ON_ERROR(epd_write_plane(0x24, image, false, "black"), TAG, "black RAM write failed");
    ESP_RETURN_ON_ERROR(epd_write_plane(0x26, image + VP_EPD_PLANE_SIZE, true, "red"), TAG, "red RAM write failed");
    ESP_RETURN_ON_ERROR(epd_refresh(), TAG, "SSD1680 refresh failed");
    ESP_LOGI(TAG, "EPD refreshed black/white/red image");
    return ESP_OK;
}

esp_err_t epd_2in13_sleep(void)
{
    ESP_RETURN_ON_ERROR(epd_wait_idle("before deep sleep"), TAG, "BUSY before sleep failed");
    ESP_RETURN_ON_ERROR(epd_send_command(0x10), TAG, "deep sleep command failed");
    ESP_RETURN_ON_ERROR(epd_send_data_byte(0x01), TAG, "deep sleep data failed");
    epd_delay_ms(100);
    ESP_LOGI(TAG, "EPD deep sleep");
    return ESP_OK;
}
