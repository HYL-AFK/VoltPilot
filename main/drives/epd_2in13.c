#include "epd_2in13.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "vp_board.h"
#include "vp_types.h"

static const char *TAG = "epd_2in13";
static spi_device_handle_t s_epd_spi;

/* 统一封装延时，避免驱动层直接依赖 Tick 细节。 */
static void epd_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static esp_err_t epd_write_byte(uint8_t value)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &value,
    };

    return spi_device_polling_transmit(s_epd_spi, &t);
}

static esp_err_t epd_send_command(uint8_t command)
{
    gpio_set_level(VP_EPD_PIN_DC, 0);
    return epd_write_byte(command);
}

static esp_err_t epd_send_data_byte(uint8_t data)
{
    gpio_set_level(VP_EPD_PIN_DC, 1);
    return epd_write_byte(data);
}

static esp_err_t epd_send_data(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return ESP_OK;
    }

    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };

    gpio_set_level(VP_EPD_PIN_DC, 1);
    return spi_device_polling_transmit(s_epd_spi, &t);
}

static esp_err_t epd_send_fill(uint8_t value, size_t len)
{
    uint8_t chunk[64];
    memset(chunk, value, sizeof(chunk));

    while (len > 0) {
        size_t send_len = len > sizeof(chunk) ? sizeof(chunk) : len;
        ESP_RETURN_ON_ERROR(epd_send_data(chunk, send_len), TAG, "send fill failed");
        len -= send_len;
    }

    return ESP_OK;
}

/* 常规等待：用于 reset、set window、power on 等阶段。 */
static esp_err_t epd_wait_idle(const char *phase)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(VP_EPD_BUSY_TIMEOUT_MS);
    int first_level = gpio_get_level(VP_EPD_PIN_BUSY);

    while (gpio_get_level(VP_EPD_PIN_BUSY) == VP_EPD_BUSY_ACTIVE_LEVEL) {
        if ((xTaskGetTickCount() - start) >= timeout) {
            ESP_LOGE(TAG, "%s BUSY timeout, level=%d", phase, gpio_get_level(VP_EPD_PIN_BUSY));
            return ESP_ERR_TIMEOUT;
        }
        epd_delay_ms(10);
    }

    uint32_t elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - start);
    ESP_LOGI(TAG, "%s BUSY first=%d idle=%d elapsed=%" PRIu32 " ms",
             phase, first_level, gpio_get_level(VP_EPD_PIN_BUSY), elapsed_ms);
    return ESP_OK;
}

/*
 * 刷新阶段单独统计是否真的出现过 BUSY。
 * 目前这块屏的核心问题是“命令发出了，但刷新没有进入忙态”，
 * 所以这里保留更细的诊断日志。
 */
static esp_err_t epd_wait_idle_after_refresh(const char *phase, bool *busy_seen)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(VP_EPD_BUSY_TIMEOUT_MS);
    int first_level = gpio_get_level(VP_EPD_PIN_BUSY);

    *busy_seen = first_level == VP_EPD_BUSY_ACTIVE_LEVEL;
    while (gpio_get_level(VP_EPD_PIN_BUSY) == VP_EPD_BUSY_ACTIVE_LEVEL) {
        if ((xTaskGetTickCount() - start) >= timeout) {
            ESP_LOGE(TAG, "%s BUSY timeout, level=%d", phase, gpio_get_level(VP_EPD_PIN_BUSY));
            return ESP_ERR_TIMEOUT;
        }
        epd_delay_ms(10);
    }

    uint32_t elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - start);
    ESP_LOGI(TAG, "%s BUSY first=%d idle=%d elapsed=%" PRIu32 " ms seen=%d",
             phase, first_level, gpio_get_level(VP_EPD_PIN_BUSY), elapsed_ms, *busy_seen);
    return ESP_OK;
}

/* 这类 122x250 屏常从最后一行开始写入，先把地址计数器归位。 */
static esp_err_t epd_set_cursor_home(void)
{
    ESP_RETURN_ON_ERROR(epd_send_command(0x4E), TAG, "set x counter cmd failed");
    ESP_RETURN_ON_ERROR(epd_send_data_byte(0x00), TAG, "set x counter failed");
    ESP_RETURN_ON_ERROR(epd_send_command(0x4F), TAG, "set y counter cmd failed");
    ESP_RETURN_ON_ERROR(epd_send_data_byte((VP_EPD_HEIGHT - 1) & 0xff), TAG, "set y counter l failed");
    ESP_RETURN_ON_ERROR(epd_send_data_byte((VP_EPD_HEIGHT - 1) >> 8), TAG, "set y counter h failed");

    return epd_wait_idle("set cursor");
}

/* 硬复位时序按更保守的脉宽处理，优先保证屏能被拉起。 */
static esp_err_t epd_reset(void)
{
    gpio_set_level(VP_EPD_PIN_RST, 1);
    epd_delay_ms(100);
    gpio_set_level(VP_EPD_PIN_RST, 0);
    epd_delay_ms(10);
    gpio_set_level(VP_EPD_PIN_RST, 1);
    epd_delay_ms(100);

    return epd_wait_idle("hardware reset");
}

/* 配置显示窗口，并把 RAM 光标移动到当前窗口首页。 */
static esp_err_t epd_set_window(void)
{
    ESP_RETURN_ON_ERROR(epd_send_command(0x44), TAG, "set x range cmd failed");
    ESP_RETURN_ON_ERROR(epd_send_data_byte(0x00), TAG, "set x range start failed");
    ESP_RETURN_ON_ERROR(epd_send_data_byte(VP_EPD_WIDTH_BYTES - 1), TAG, "set x range end failed");

    ESP_RETURN_ON_ERROR(epd_send_command(0x45), TAG, "set y range cmd failed");
    ESP_RETURN_ON_ERROR(epd_send_data_byte(0x00), TAG, "set y range start l failed");
    ESP_RETURN_ON_ERROR(epd_send_data_byte(0x00), TAG, "set y range start h failed");
    ESP_RETURN_ON_ERROR(epd_send_data_byte((VP_EPD_HEIGHT - 1) & 0xff), TAG, "set y range end l failed");
    ESP_RETURN_ON_ERROR(epd_send_data_byte((VP_EPD_HEIGHT - 1) >> 8), TAG, "set y range end h failed");

    ESP_RETURN_ON_ERROR(epd_set_cursor_home(), TAG, "set cursor home failed");
    return epd_wait_idle("set window");
}

/*
 * 刷新流程保留多组候选命令，方便适配不同批次的 2.13 寸三色屏。
 * 如果所有命令都不触发 BUSY，基本可以判断当前控制器序列不匹配。
 */
static esp_err_t epd_refresh(void)
{
    static const uint8_t update_controls[] = {
        0xF7, /* 常见 SSD1680 全刷参数。 */
        0xC7, /* 部分 2.13 寸屏使用的替代全刷参数。 */
        0xFF, /* 对 UC8151/SSD168x 兼容屏更激进的刷新参数。 */
    };

    ESP_LOGI(TAG, "refresh try power on 0x04");
    ESP_RETURN_ON_ERROR(epd_send_command(0x04), TAG, "power on cmd failed");
    ESP_RETURN_ON_ERROR(epd_wait_idle("power on"), TAG, "power on busy failed");

    for (size_t i = 0; i < sizeof(update_controls) / sizeof(update_controls[0]); i++) {
        char phase[32];
        bool busy_seen = false;

        ESP_LOGI(TAG, "refresh try update_control=0x%02X", update_controls[i]);
        ESP_RETURN_ON_ERROR(epd_send_command(0x22), TAG, "display update control cmd failed");
        ESP_RETURN_ON_ERROR(epd_send_data_byte(update_controls[i]), TAG, "display update control data failed");
        ESP_RETURN_ON_ERROR(epd_send_command(0x20), TAG, "master activate failed");
        epd_delay_ms(20);

        snprintf(phase, sizeof(phase), "refresh 0x%02X", update_controls[i]);
        ESP_RETURN_ON_ERROR(epd_wait_idle_after_refresh(phase, &busy_seen), TAG, "refresh busy failed");
        if (busy_seen) {
            return ESP_OK;
        }
    }

    {
        bool busy_seen = false;

        ESP_LOGI(TAG, "refresh try direct command 0x12");
        ESP_RETURN_ON_ERROR(epd_send_command(0x12), TAG, "direct refresh cmd failed");
        epd_delay_ms(20);
        ESP_RETURN_ON_ERROR(epd_wait_idle_after_refresh("refresh 0x12", &busy_seen), TAG, "direct refresh busy failed");
        if (busy_seen) {
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "refresh command did not trigger BUSY; panel may need another controller sequence");
    return ESP_OK;
}

esp_err_t epd_2in13_init(void)
{
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

    gpio_config_t output_cfg = {
        .pin_bit_mask = (1ULL << VP_EPD_PIN_DC) | (1ULL << VP_EPD_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output_cfg), TAG, "gpio output config failed");

    gpio_config_t input_cfg = {
        .pin_bit_mask = (1ULL << VP_EPD_PIN_BUSY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&input_cfg), TAG, "gpio input config failed");

    /* 启动时打印一次关键引脚和驱动判定，便于现场核对接线。 */
    ESP_LOGI(TAG, "EPD pins SCK=%d MOSI=%d CS=%d DC=%d RST=%d BUSY=%d busy_active=%d spi=%d Hz",
             VP_EPD_PIN_SCK, VP_EPD_PIN_MOSI, VP_EPD_PIN_CS, VP_EPD_PIN_DC,
             VP_EPD_PIN_RST, VP_EPD_PIN_BUSY, VP_EPD_BUSY_ACTIVE_LEVEL, VP_EPD_SPI_CLOCK_HZ);

    ESP_RETURN_ON_ERROR(epd_reset(), TAG, "reset failed");

    ESP_RETURN_ON_ERROR(epd_send_command(0x12), TAG, "sw reset cmd failed");
    epd_delay_ms(10);
    ESP_RETURN_ON_ERROR(epd_wait_idle("software reset"), TAG, "sw reset busy failed");

    ESP_RETURN_ON_ERROR(epd_send_command(0x01), TAG, "driver output cmd failed");
    ESP_RETURN_ON_ERROR(epd_send_data_byte((VP_EPD_HEIGHT - 1) & 0xff), TAG, "driver output h l failed");
    ESP_RETURN_ON_ERROR(epd_send_data_byte((VP_EPD_HEIGHT - 1) >> 8), TAG, "driver output h h failed");
    ESP_RETURN_ON_ERROR(epd_send_data_byte(0x00), TAG, "driver output gate failed");

    ESP_RETURN_ON_ERROR(epd_send_command(0x11), TAG, "data entry cmd failed");
    ESP_RETURN_ON_ERROR(epd_send_data_byte(0x03), TAG, "data entry data failed");

    ESP_RETURN_ON_ERROR(epd_set_window(), TAG, "set window failed");

    ESP_RETURN_ON_ERROR(epd_send_command(0x21), TAG, "display update mode cmd failed");
    ESP_RETURN_ON_ERROR(epd_send_data_byte(0x00), TAG, "display update mode data 0 failed");
    ESP_RETURN_ON_ERROR(epd_send_data_byte(0x80), TAG, "display update mode data 1 failed");

    ESP_RETURN_ON_ERROR(epd_send_command(0x3C), TAG, "border cmd failed");
    ESP_RETURN_ON_ERROR(epd_send_data_byte(0x05), TAG, "border data failed");

    ESP_RETURN_ON_ERROR(epd_send_command(0x18), TAG, "temp sensor cmd failed");
    ESP_RETURN_ON_ERROR(epd_send_data_byte(0x80), TAG, "temp sensor data failed");

    ESP_LOGI(TAG, "EPD initialized");
    return ESP_OK;
}

esp_err_t epd_2in13_clear(void)
{
    ESP_RETURN_ON_ERROR(epd_set_window(), TAG, "set window for clear failed");

    ESP_RETURN_ON_ERROR(epd_send_command(0x24), TAG, "black ram cmd failed");
    ESP_RETURN_ON_ERROR(epd_send_fill(0xFF, VP_EPD_BUFFER_SIZE), TAG, "black ram clear failed");

    ESP_RETURN_ON_ERROR(epd_send_command(0x26), TAG, "red ram cmd failed");
    ESP_RETURN_ON_ERROR(epd_send_fill(0xFF, VP_EPD_BUFFER_SIZE), TAG, "red ram clear failed");

    ESP_RETURN_ON_ERROR(epd_refresh(), TAG, "clear refresh failed");
    ESP_LOGI(TAG, "EPD cleared");
    return ESP_OK;
}

esp_err_t epd_2in13_display_bw(const uint8_t *black, const uint8_t *red)
{
    ESP_RETURN_ON_FALSE(black != NULL, ESP_ERR_INVALID_ARG, TAG, "black buffer is null");

    ESP_RETURN_ON_ERROR(epd_set_window(), TAG, "set window for display failed");

    ESP_RETURN_ON_ERROR(epd_send_command(0x24), TAG, "black ram cmd failed");
    ESP_RETURN_ON_ERROR(epd_send_data(black, VP_EPD_BUFFER_SIZE), TAG, "black ram write failed");

    ESP_RETURN_ON_ERROR(epd_send_command(0x26), TAG, "red ram cmd failed");
    if (red != NULL) {
        ESP_RETURN_ON_ERROR(epd_send_data(red, VP_EPD_BUFFER_SIZE), TAG, "red ram write failed");
    } else {
        ESP_RETURN_ON_ERROR(epd_send_fill(0xFF, VP_EPD_BUFFER_SIZE), TAG, "red ram blank failed");
    }

    ESP_RETURN_ON_ERROR(epd_refresh(), TAG, "display refresh failed");
    ESP_LOGI(TAG, "EPD refreshed");
    return ESP_OK;
}

esp_err_t epd_2in13_sleep(void)
{
    ESP_RETURN_ON_ERROR(epd_send_command(0x10), TAG, "deep sleep cmd failed");
    ESP_RETURN_ON_ERROR(epd_send_data_byte(0x01), TAG, "deep sleep data failed");
    epd_delay_ms(100);

    ESP_LOGI(TAG, "EPD sleep");
    return ESP_OK;
}
