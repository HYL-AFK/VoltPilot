#include "bms_service.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_state_service.h"
#include "bms_protocol.h"
#include "diag_service.h"
#include "watchdog_service.h"
#include "vp_board.h"

#define BMS_RX_BUF_SIZE 512
#define BMS_TX_BUF_SIZE 256
#define BMS_RX_CHUNK_SIZE 128
#define BMS_FRAME_BUFFER_SIZE (BMS_PROTOCOL_MAX_FRAME_LEN * 2)

static const char *TAG = "bms_service";

static bms_info_t s_bms_info;
static uint8_t s_frame_buffer[BMS_FRAME_BUFFER_SIZE];
static size_t s_frame_len;

bool bms_service_get_info(bms_info_t *out_info)
{
    if (out_info == NULL) {
        return false;
    }
    *out_info = s_bms_info;
    return s_bms_info.online && s_bms_info.parsed_valid;
}

static void log_hex_frame(const char *prefix, const uint8_t *data, int len)
{
    char line[3 * 48 + 1];
    int offset = 0;
    int show_len = len > 48 ? 48 : len;

    for (int i = 0; i < show_len && offset < (int)sizeof(line); i++) {
        offset += snprintf(line + offset, sizeof(line) - offset, "%02X ", data[i]);
    }
    line[sizeof(line) - 1] = '\0';
    ESP_LOGI(TAG, "%s len=%d data=%s%s", prefix, len, line, len > 48 ? "..." : "");
}

static esp_err_t bms_uart_init(void)
{
    uart_config_t cfg = {
        .baud_rate = VP_BMS_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(VP_BMS_UART_PORT, BMS_RX_BUF_SIZE, BMS_TX_BUF_SIZE, 0, NULL, 0),
                        TAG, "安装 BMS UART 驱动失败");
    ESP_RETURN_ON_ERROR(uart_param_config(VP_BMS_UART_PORT, &cfg), TAG, "配置 BMS UART 参数失败");
    ESP_RETURN_ON_ERROR(uart_set_pin(VP_BMS_UART_PORT, VP_BMS_PIN_TX, VP_BMS_PIN_RX,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "配置 BMS UART 引脚失败");

    ESP_LOGI(TAG, "RS485 UART%d RX=%d TX=%d baud=%d request=%s interval=%dms",
             VP_BMS_UART_PORT, VP_BMS_PIN_RX, VP_BMS_PIN_TX, VP_BMS_UART_BAUD_RATE,
             BMS_PROTOCOL_REQUEST_TEXT, VP_BMS_REQUEST_INTERVAL_MS);
    return ESP_OK;
}

static void bms_send_request(void)
{
    const char request[] = BMS_PROTOCOL_REQUEST_TEXT;
    int written = uart_write_bytes(VP_BMS_UART_PORT, request, strlen(request));

    if (written == (int)strlen(request)) {
        ESP_LOGI(TAG, "BMS TX request=%s", request);
    } else {
        ESP_LOGW(TAG, "BMS TX request failed written=%d", written);
    }
}

static int find_header(const uint8_t *data, size_t len)
{
    if (len < BMS_PROTOCOL_HEADER_LEN) {
        return -1;
    }

    for (size_t i = 0; i <= len - BMS_PROTOCOL_HEADER_LEN; i++) {
        if (memcmp(data + i, BMS_PROTOCOL_HEADER_TEXT, BMS_PROTOCOL_HEADER_LEN) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static size_t header_prefix_keep_len(const uint8_t *data, size_t len)
{
    size_t max_keep = len < BMS_PROTOCOL_HEADER_LEN - 1 ? len : BMS_PROTOCOL_HEADER_LEN - 1;

    for (size_t keep = max_keep; keep > 0; keep--) {
        if (memcmp(data + len - keep, BMS_PROTOCOL_HEADER_TEXT, keep) == 0) {
            return keep;
        }
    }
    return 0;
}

static void consume_frame_bytes(size_t count)
{
    if (count >= s_frame_len) {
        s_frame_len = 0;
        return;
    }

    memmove(s_frame_buffer, s_frame_buffer + count, s_frame_len - count);
    s_frame_len -= count;
}

static void append_rx_bytes(const uint8_t *data, size_t len)
{
    if (len > sizeof(s_frame_buffer)) {
        data += len - sizeof(s_frame_buffer);
        len = sizeof(s_frame_buffer);
        s_frame_len = 0;
    }

    if (s_frame_len + len > sizeof(s_frame_buffer)) {
        size_t drop = s_frame_len + len - sizeof(s_frame_buffer);
        ESP_LOGW(TAG, "BMS RX buffer overflow, drop=%u", (unsigned)drop);
        consume_frame_bytes(drop);
    }

    memcpy(s_frame_buffer + s_frame_len, data, len);
    s_frame_len += len;
}

static esp_err_t parse_frame_candidate(size_t frame_len, bms_info_t *parsed)
{
    *parsed = s_bms_info;
    esp_err_t err = bms_protocol_parse(s_frame_buffer, frame_len, parsed);

    if (err == ESP_OK) {
        uint32_t now_ms = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
        parsed->online = true;
        parsed->parsed_valid = true;
        parsed->rx_frames = s_bms_info.rx_frames + 1;
        parsed->parsed_frames = s_bms_info.parsed_frames + 1;
        parsed->timeout_count = s_bms_info.timeout_count;
        parsed->crc_error_count = s_bms_info.crc_error_count;
        parsed->parse_error_count = s_bms_info.parse_error_count;
        parsed->last_rx_ms = now_ms;
        parsed->last_parse_ms = now_ms;
    }
    return err;
}

static void log_parse_error(esp_err_t err, const bms_info_t *candidate, size_t frame_len)
{
    if (err == ESP_ERR_INVALID_CRC) {
        s_bms_info.crc_error_count++;
        ESP_LOGW(TAG, "BMS CRC fail len=%u calc=0x%04X rx_le=0x%04X rx_be=0x%04X count=%" PRIu32,
                 (unsigned)frame_len, candidate->last_crc_calc, candidate->last_crc_rx_le,
                 candidate->last_crc_rx_be, s_bms_info.crc_error_count);
    } else {
        s_bms_info.parse_error_count++;
        ESP_LOGW(TAG, "BMS parse fail len=%u err=%s count=%" PRIu32,
                 (unsigned)frame_len, esp_err_to_name(err), s_bms_info.parse_error_count);
    }
    (void)diag_service_update_bms(&s_bms_info);
}

static void accept_parsed_frame(const bms_info_t *parsed, size_t frame_len)
{
    s_bms_info = *parsed;
    log_hex_frame("BMS RX frame", s_frame_buffer, (int)frame_len);
    ESP_LOGI(TAG,
             "BMS parsed material=%s softid=%s cell=%u pack=%" PRId32 "mV current=%" PRId32
             "mA rsoc=%u%% asoc=%u%% soh=%u%% protect=%02X/%02X/%02X",
             bms_material_name(s_bms_info.material), s_bms_info.soft_id, s_bms_info.cell_count,
             s_bms_info.pack_mv, s_bms_info.current_ma, s_bms_info.rsoc_percent,
             s_bms_info.asoc_percent, s_bms_info.soh_percent, s_bms_info.protect_1,
             s_bms_info.protect_2, s_bms_info.protect_3);
    (void)app_state_post_event(VP_APP_EVENT_BMS_RX, (int32_t)frame_len);
    (void)diag_service_update_bms(&s_bms_info);
    consume_frame_bytes(frame_len);
}

static bool try_parse_known_length(size_t frame_len)
{
    bms_info_t parsed;
    esp_err_t err = parse_frame_candidate(frame_len, &parsed);

    if (err == ESP_OK) {
        accept_parsed_frame(&parsed, frame_len);
        return true;
    }

    log_parse_error(err, &parsed, frame_len);
    consume_frame_bytes(1);
    return true;
}

static void process_rx_buffer(void)
{
    while (s_frame_len > 0) {
        int header_pos = find_header(s_frame_buffer, s_frame_len);
        if (header_pos < 0) {
            size_t keep = header_prefix_keep_len(s_frame_buffer, s_frame_len);
            if (keep > 0) {
                memmove(s_frame_buffer, s_frame_buffer + s_frame_len - keep, keep);
            }
            s_frame_len = keep;
            return;
        }

        if (header_pos > 0) {
            consume_frame_bytes((size_t)header_pos);
        }

        size_t expected_len = bms_protocol_expected_len_from_header(s_frame_buffer, s_frame_len);
        if (expected_len != 0) {
            if (s_frame_len < expected_len) {
                return;
            }
            (void)try_parse_known_length(expected_len);
            continue;
        }

        if (s_frame_len < BMS_PROTOCOL_FRAME_LEN_14S) {
            return;
        }

        bms_info_t parsed14;
        esp_err_t err14 = parse_frame_candidate(BMS_PROTOCOL_FRAME_LEN_14S, &parsed14);
        if (err14 == ESP_OK) {
            accept_parsed_frame(&parsed14, BMS_PROTOCOL_FRAME_LEN_14S);
            continue;
        }

        if (s_frame_len < BMS_PROTOCOL_FRAME_LEN_15S) {
            return;
        }

        bms_info_t parsed15;
        esp_err_t err15 = parse_frame_candidate(BMS_PROTOCOL_FRAME_LEN_15S, &parsed15);
        if (err15 == ESP_OK) {
            accept_parsed_frame(&parsed15, BMS_PROTOCOL_FRAME_LEN_15S);
            continue;
        }

        log_parse_error(err14 == ESP_ERR_INVALID_CRC ? err15 : err14,
                        err15 == ESP_ERR_INVALID_CRC ? &parsed15 : &parsed14,
                        BMS_PROTOCOL_FRAME_LEN_15S);
        consume_frame_bytes(1);
    }
}

static void bms_service_task(void *arg)
{
    (void)arg;
    uint8_t data[BMS_RX_CHUNK_SIZE];
    TickType_t last_rx_tick = xTaskGetTickCount();
    TickType_t last_request_tick = 0;
    (void)watchdog_service_subscribe_current_task("vp_bms");

    while (true) {
        TickType_t now = xTaskGetTickCount();
        if (last_request_tick == 0 ||
            pdTICKS_TO_MS(now - last_request_tick) >= VP_BMS_REQUEST_INTERVAL_MS) {
            bms_send_request();
            last_request_tick = now;
        }

        int len = uart_read_bytes(VP_BMS_UART_PORT, data, sizeof(data), pdMS_TO_TICKS(100));
        if (len > 0) {
            last_rx_tick = xTaskGetTickCount();
            s_bms_info.online = true;
            s_bms_info.last_rx_ms = (uint32_t)pdTICKS_TO_MS(last_rx_tick);
            log_hex_frame("BMS RX chunk", data, len);
            append_rx_bytes(data, (size_t)len);
            process_rx_buffer();
        }

        if (pdTICKS_TO_MS(xTaskGetTickCount() - last_rx_tick) >= VP_BMS_RX_TIMEOUT_MS) {
            last_rx_tick = xTaskGetTickCount();
            s_bms_info.online = false;
            s_bms_info.parsed_valid = false;
            s_bms_info.timeout_count++;
            s_frame_len = 0;
            ESP_LOGW(TAG, "BMS 接收超时 count=%" PRIu32, s_bms_info.timeout_count);
            (void)diag_service_update_bms(&s_bms_info);
            (void)app_state_post_event(VP_APP_EVENT_BMS_TIMEOUT, s_bms_info.timeout_count);
        }
        watchdog_service_feed();
    }
}

esp_err_t bms_service_init(void)
{
    memset(&s_bms_info, 0, sizeof(s_bms_info));
    s_frame_len = 0;
    ESP_RETURN_ON_ERROR(bms_uart_init(), TAG, "BMS UART 初始化失败");

    BaseType_t ok = xTaskCreate(bms_service_task, "vp_bms", VP_TASK_STACK_LARGE, NULL,
                                VP_TASK_PRIO_NORMAL, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "创建 BMS 任务失败");
    ESP_LOGI(TAG, "BMS 服务初始化完成，已启用出海版本特殊协议解析");
    return ESP_OK;
}
