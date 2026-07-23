#include "stc_service.h"

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
#include "diag_service.h"
#include "stc_protocol.h"
#include "watchdog_service.h"
#include "vp_board.h"

#define STC_RX_BUF_SIZE 512
#define STC_TX_BUF_SIZE 256
#define STC_RX_CHUNK_SIZE 96
#define STC_STREAM_BUFFER_SIZE (STC_PROTOCOL_MAX_FRAME_LEN * 4)

static const char *TAG = "stc_service";

/* 三挡开关联调阶段：只显示 READ_GEAR 和挡位解析结果。 */
#define VP_STC_GEAR_LOG_ONLY 1

static stc_info_t s_stc_info;
static uint8_t s_stream_buffer[STC_STREAM_BUFFER_SIZE];
static size_t s_stream_len;
static uint8_t s_next_seq;

bool stc_service_version_compatible(const stc_info_t *info)
{
    return info != NULL &&
           info->protocol_version == VP_STC_REQUIRED_PROTOCOL_VERSION &&
           info->firmware_major >= VP_STC_REQUIRED_FIRMWARE_MAJOR &&
           info->hardware_major >= VP_STC_REQUIRED_HARDWARE_MAJOR;
}

#if VP_ENABLE_MOCK_DEVICES
static void stc_mock_task(void *arg)
{
    (void)arg;
    (void)watchdog_service_subscribe_current_task("vp_stc_mock");
    memset(&s_stc_info, 0, sizeof(s_stc_info));
    s_stc_info.raw_gear = 1;
    s_stc_info.gear_valid = true;
    s_stc_info.adc_key_raw = 1000;
    s_stc_info.debounce_ms = 50;
    s_stc_info.protocol_version = 1;
    s_stc_info.firmware_major = 1;
    s_stc_info.hardware_major = 1;

    while (true) {
        s_stc_info.online = VP_MOCK_STC_FAULT_MODE == 0;
        s_stc_info.gear_valid = VP_MOCK_STC_FAULT_MODE == 0;
        s_stc_info.uptime_ms = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
        s_stc_info.rx_frames++;
        s_stc_info.parsed_frames++;
        (void)diag_service_update_stc(&s_stc_info);
        if (s_stc_info.online) {
            (void)app_state_post_event(VP_APP_EVENT_STC_RX, STC_FUNC_READ_GEAR);
        } else {
            s_stc_info.timeout_count++;
            (void)app_state_post_event(VP_APP_EVENT_STC_TIMEOUT, s_stc_info.timeout_count);
        }
        watchdog_service_feed();
        vTaskDelay(pdMS_TO_TICKS(VP_STC_READ_GEAR_INTERVAL_MS));
    }
}
#endif

bool stc_service_get_info(stc_info_t *out_info)
{
    if (out_info == NULL) {
        return false;
    }
    *out_info = s_stc_info;
    return s_stc_info.online;
}

static uint16_t read_be16(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8) | data[1];
}

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           data[3];
}

static void log_hex_frame(const char *prefix, const uint8_t *data, int len)
{
#if VP_STC_GEAR_LOG_ONLY
    (void)prefix;
    (void)data;
    (void)len;
    return;
#else
    char line[3 * 48 + 1];
    int offset = 0;
    int show_len = len > 48 ? 48 : len;

    for (int i = 0; i < show_len && offset < (int)sizeof(line); i++) {
        offset += snprintf(line + offset, sizeof(line) - offset, "%02X ", data[i]);
    }
    line[sizeof(line) - 1] = '\0';
    ESP_LOGI(TAG, "%s len=%d data=%s%s", prefix, len, line, len > 48 ? "..." : "");
#endif
}

static bool stc_addr_can_start_frame(uint8_t addr)
{
    return addr == STC_PROTOCOL_SLAVE_ADDR;
}

static void consume_stream_bytes(size_t count)
{
    if (count >= s_stream_len) {
        s_stream_len = 0;
        return;
    }

    memmove(s_stream_buffer, s_stream_buffer + count, s_stream_len - count);
    s_stream_len -= count;
}

static void append_rx_bytes(const uint8_t *data, size_t len)
{
    if (len > sizeof(s_stream_buffer)) {
        data += len - sizeof(s_stream_buffer);
        len = sizeof(s_stream_buffer);
        s_stream_len = 0;
    }

    if (s_stream_len + len > sizeof(s_stream_buffer)) {
        size_t drop = s_stream_len + len - sizeof(s_stream_buffer);
        ESP_LOGW(TAG, "STC RX buffer overflow, drop=%u", (unsigned)drop);
        consume_stream_bytes(drop);
    }

    memcpy(s_stream_buffer + s_stream_len, data, len);
    s_stream_len += len;
}

static esp_err_t stc_uart_init(void)
{
    uart_config_t cfg = {
        .baud_rate = VP_STC_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(VP_STC_UART_PORT, STC_RX_BUF_SIZE, STC_TX_BUF_SIZE, 0, NULL, 0),
                        TAG, "安装 STC UART 驱动失败");
    ESP_RETURN_ON_ERROR(uart_param_config(VP_STC_UART_PORT, &cfg), TAG, "配置 STC UART 参数失败");
    ESP_RETURN_ON_ERROR(uart_set_pin(VP_STC_UART_PORT, VP_STC_PIN_TX, VP_STC_PIN_RX,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "配置 STC UART 引脚失败");

    ESP_LOGI(TAG, "STC UART%d TX=%d RX=%d baud=%d addr_host=0x%02X addr_stc=0x%02X",
             VP_STC_UART_PORT, VP_STC_PIN_TX, VP_STC_PIN_RX, VP_STC_UART_BAUD_RATE,
             STC_PROTOCOL_HOST_ADDR, STC_PROTOCOL_SLAVE_ADDR);
    return ESP_OK;
}

static void stc_send_request(uint8_t func)
{
    uint8_t frame[STC_PROTOCOL_MAX_FRAME_LEN];
    size_t frame_len = 0;
    uint8_t seq = s_next_seq++;

    esp_err_t err = stc_protocol_build_request(STC_PROTOCOL_SLAVE_ADDR, func, seq, NULL, 0,
                                               frame, sizeof(frame), &frame_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STC build request failed func=%s err=%s",
                 stc_protocol_func_name(func), esp_err_to_name(err));
        return;
    }

    int written = uart_write_bytes(VP_STC_UART_PORT, frame, frame_len);
    if (written == (int)frame_len) {
#if !VP_STC_GEAR_LOG_ONLY
        ESP_LOGI(TAG, "STC TX %s seq=%u len=%u", stc_protocol_func_name(func), seq, (unsigned)frame_len);
#else
        if (func == STC_FUNC_READ_GEAR) {
            ESP_LOGI(TAG, "STC TX READ_GEAR seq=%u len=%u", seq, (unsigned)frame_len);
        }
#endif
    } else {
        ESP_LOGW(TAG, "STC TX failed func=%s seq=%u written=%d/%u",
                 stc_protocol_func_name(func), seq, written, (unsigned)frame_len);
    }
}

static void stc_poll_requests(TickType_t now,
                              TickType_t *last_heartbeat,
                              TickType_t *last_gear,
                              TickType_t *last_io,
                              TickType_t *last_version)
{
    if (*last_heartbeat == 0 ||
        pdTICKS_TO_MS(now - *last_heartbeat) >= VP_STC_HEARTBEAT_INTERVAL_MS) {
        stc_send_request(STC_FUNC_HEARTBEAT);
        *last_heartbeat = now;
        return;
    }

    if (*last_gear == 0 ||
        pdTICKS_TO_MS(now - *last_gear) >= VP_STC_READ_GEAR_INTERVAL_MS) {
        stc_send_request(STC_FUNC_READ_GEAR);
        *last_gear = now;
        return;
    }

    if (*last_io == 0 ||
        pdTICKS_TO_MS(now - *last_io) >= VP_STC_READ_IO_INTERVAL_MS) {
        stc_send_request(STC_FUNC_READ_IO_STATUS);
        *last_io = now;
        return;
    }

    if (*last_version == 0 ||
        pdTICKS_TO_MS(now - *last_version) >= VP_STC_READ_VERSION_INTERVAL_MS) {
        stc_send_request(STC_FUNC_READ_VERSION);
        *last_version = now;
    }
}

static void mark_online_frame(void)
{
    uint32_t now_ms = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());

    s_stc_info.online = true;
    s_stc_info.rx_frames++;
    s_stc_info.last_parse_ms = now_ms;
}

static void post_parsed_frame(uint8_t func)
{
    (void)diag_service_update_stc(&s_stc_info);
    (void)app_state_post_event(VP_APP_EVENT_STC_RX, func);
}

static bool parse_payload_error(const stc_protocol_frame_t *frame, const char *reason)
{
    s_stc_info.parse_error_count++;
    ESP_LOGW(TAG, "STC parse fail func=%s seq=%u len=%u reason=%s count=%" PRIu32,
             stc_protocol_func_name(frame->func), frame->seq, frame->payload_len,
             reason, s_stc_info.parse_error_count);
    (void)diag_service_update_stc(&s_stc_info);
    return true;
}

static bool handle_exception_frame(const stc_protocol_frame_t *frame)
{
    uint8_t error_code = frame->payload_len > 0 ? frame->payload[0] : 0;

    mark_online_frame();
    s_stc_info.parse_error_count++;
    ESP_LOGW(TAG, "STC RX exception func=%s seq=%u code=0x%02X(%s) count=%" PRIu32,
             stc_protocol_func_name(frame->func), frame->seq, error_code,
             stc_protocol_error_name(error_code), s_stc_info.parse_error_count);
    (void)diag_service_update_stc(&s_stc_info);
    return true;
}

static bool handle_heartbeat_frame(const stc_protocol_frame_t *frame)
{
    if (frame->payload_len < 7) {
        return parse_payload_error(frame, "heartbeat payload too short");
    }

    s_stc_info.uptime_ms = read_be32(frame->payload);
    s_stc_info.status_flags = read_be16(frame->payload + 4);
    s_stc_info.protocol_version = frame->payload[6];
    s_stc_info.parsed_frames++;
    post_parsed_frame(frame->func);
#if !VP_STC_GEAR_LOG_ONLY
    ESP_LOGI(TAG, "STC RX HEARTBEAT seq=%u uptime=%" PRIu32 "ms status=0x%04X proto=%u",
             frame->seq, s_stc_info.uptime_ms, s_stc_info.status_flags,
             s_stc_info.protocol_version);
#endif
    return true;
}

static bool handle_gear_frame(const stc_protocol_frame_t *frame)
{
    if (frame->payload_len < 6) {
        return parse_payload_error(frame, "gear payload too short");
    }

    s_stc_info.raw_gear = frame->payload[0];
    s_stc_info.gear_valid = frame->payload[1] != 0;
    s_stc_info.adc_key_raw = read_be16(frame->payload + 2);
    s_stc_info.debounce_ms = read_be16(frame->payload + 4);
    s_stc_info.parsed_frames++;
    post_parsed_frame(frame->func);
    ESP_LOGI(TAG, "STC parsed gear=%u valid=%d adc_key_raw=%u debounce=%ums",
             s_stc_info.raw_gear, s_stc_info.gear_valid,
             s_stc_info.adc_key_raw, s_stc_info.debounce_ms);
    return true;
}

static bool handle_io_status_frame(const stc_protocol_frame_t *frame)
{
    if (frame->payload_len < 4) {
        return parse_payload_error(frame, "io payload too short");
    }

    s_stc_info.io_inputs = read_be16(frame->payload);
    s_stc_info.io_outputs = read_be16(frame->payload + 2);
    s_stc_info.io_status_valid = true;
    s_stc_info.parsed_frames++;
    post_parsed_frame(frame->func);
#if !VP_STC_GEAR_LOG_ONLY
    ESP_LOGI(TAG, "STC RX IO_STATUS seq=%u inputs=0x%04X outputs=0x%04X",
             frame->seq, s_stc_info.io_inputs, s_stc_info.io_outputs);
#endif
    return true;
}

static bool handle_version_frame(const stc_protocol_frame_t *frame)
{
    if (frame->payload_len < 6) {
        return parse_payload_error(frame, "version payload too short");
    }

    s_stc_info.protocol_version = frame->payload[0];
    s_stc_info.firmware_major = frame->payload[1];
    s_stc_info.firmware_minor = frame->payload[2];
    s_stc_info.firmware_patch = frame->payload[3];
    s_stc_info.hardware_major = frame->payload[4];
    s_stc_info.hardware_minor = frame->payload[5];
    s_stc_info.parsed_frames++;
    post_parsed_frame(frame->func);
#if !VP_STC_GEAR_LOG_ONLY
    ESP_LOGI(TAG, "STC RX VERSION seq=%u proto=%u fw=%u.%u.%u hw=%u.%u",
             frame->seq, s_stc_info.protocol_version,
             s_stc_info.firmware_major, s_stc_info.firmware_minor, s_stc_info.firmware_patch,
             s_stc_info.hardware_major, s_stc_info.hardware_minor);
#endif
    return true;
}

static bool handle_control_ack_frame(const stc_protocol_frame_t *frame)
{
    uint8_t status = frame->payload_len > 0 ? frame->payload[0] : 0;

    s_stc_info.parsed_frames++;
    post_parsed_frame(frame->func);
    ESP_LOGI(TAG, "STC RX WRITE_CONTROL ack seq=%u status=0x%02X", frame->seq, status);
    return true;
}

static bool handle_valid_frame(const stc_protocol_frame_t *frame)
{
    if (!stc_addr_can_start_frame(frame->addr)) {
        s_stc_info.parse_error_count++;
        ESP_LOGW(TAG, "STC drop frame with unexpected addr=0x%02X count=%" PRIu32,
                 frame->addr, s_stc_info.parse_error_count);
        return false;
    }

    log_hex_frame("STC RX frame", s_stream_buffer, (int)frame->frame_len);

    if (stc_protocol_is_exception_func(frame->func)) {
        return handle_exception_frame(frame);
    }

    mark_online_frame();
    switch (frame->func) {
    case STC_FUNC_HEARTBEAT:
        return handle_heartbeat_frame(frame);
    case STC_FUNC_READ_GEAR:
        return handle_gear_frame(frame);
    case STC_FUNC_READ_IO_STATUS:
        return handle_io_status_frame(frame);
    case STC_FUNC_WRITE_CONTROL:
        return handle_control_ack_frame(frame);
    case STC_FUNC_READ_VERSION:
        return handle_version_frame(frame);
    default:
        return parse_payload_error(frame, "unsupported function");
    }
}

static bool process_stream_buffer(void)
{
    bool accepted_valid = false;

    while (s_stream_len >= STC_PROTOCOL_MIN_FRAME_LEN) {
        if (!stc_addr_can_start_frame(s_stream_buffer[0])) {
            consume_stream_bytes(1);
            continue;
        }

        stc_protocol_frame_t frame = {0};
        size_t consumed = 0;
        esp_err_t err = stc_protocol_try_parse(s_stream_buffer, s_stream_len, &frame, &consumed);

        if (err == ESP_ERR_TIMEOUT) {
            return accepted_valid;
        }

        if (err == ESP_ERR_INVALID_CRC) {
            s_stc_info.crc_error_count++;
            ESP_LOGW(TAG, "STC CRC fail frame_len=%u calc=0x%04X rx=0x%04X count=%" PRIu32,
                     (unsigned)frame.frame_len, frame.crc_calc, frame.crc_rx,
                     s_stc_info.crc_error_count);
            (void)diag_service_update_stc(&s_stc_info);
            consume_stream_bytes(consumed == 0 ? 1 : consumed);
            continue;
        }

        if (err != ESP_OK) {
            s_stc_info.parse_error_count++;
            ESP_LOGW(TAG, "STC parse fail err=%s count=%" PRIu32,
                     esp_err_to_name(err), s_stc_info.parse_error_count);
            (void)diag_service_update_stc(&s_stc_info);
            consume_stream_bytes(consumed == 0 ? 1 : consumed);
            continue;
        }

        accepted_valid |= handle_valid_frame(&frame);
        consume_stream_bytes(consumed);
    }

    return accepted_valid;
}

static void stc_service_task(void *arg)
{
    (void)arg;
    uint8_t data[STC_RX_CHUNK_SIZE];
    TickType_t last_valid_tick = xTaskGetTickCount();
    TickType_t last_heartbeat = 0;
    TickType_t last_gear = 0;
    TickType_t last_io = 0;
    TickType_t last_version = 0;
    (void)watchdog_service_subscribe_current_task("vp_stc");

    while (true) {
        TickType_t now = xTaskGetTickCount();
        stc_poll_requests(now, &last_heartbeat, &last_gear, &last_io, &last_version);

        int len = uart_read_bytes(VP_STC_UART_PORT, data, sizeof(data), pdMS_TO_TICKS(100));
        if (len > 0) {
            TickType_t rx_tick = xTaskGetTickCount();
            s_stc_info.last_rx_ms = (uint32_t)pdTICKS_TO_MS(rx_tick);
            log_hex_frame("STC RX chunk", data, len);
            append_rx_bytes(data, (size_t)len);
            if (process_stream_buffer()) {
                last_valid_tick = xTaskGetTickCount();
            }
        }

        if (pdTICKS_TO_MS(xTaskGetTickCount() - last_valid_tick) >= VP_STC_RX_TIMEOUT_MS) {
            last_valid_tick = xTaskGetTickCount();
            s_stc_info.online = false;
            s_stc_info.gear_valid = false;
            s_stc_info.io_status_valid = false;
            s_stc_info.timeout_count++;
            s_stream_len = 0;
            ESP_LOGW(TAG, "STC_TIMEOUT count=%" PRIu32, s_stc_info.timeout_count);
            (void)diag_service_update_stc(&s_stc_info);
            (void)app_state_post_event(VP_APP_EVENT_STC_TIMEOUT, s_stc_info.timeout_count);
        }
        watchdog_service_feed();
    }
}

esp_err_t stc_service_init(void)
{
    memset(&s_stc_info, 0, sizeof(s_stc_info));
    s_stream_len = 0;
    s_next_seq = 0;
#if VP_ENABLE_MOCK_DEVICES
    BaseType_t ok = xTaskCreate(stc_mock_task, "vp_stc_mock", VP_TASK_STACK_LARGE, NULL,
                                VP_TASK_PRIO_NORMAL, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "创建 STC mock 任务失败");
    ESP_LOGW(TAG, "STC mock 已启用，未打开 UART");
    return ESP_OK;
#else
    ESP_RETURN_ON_ERROR(stc_uart_init(), TAG, "STC UART 初始化失败");

    BaseType_t ok = xTaskCreate(stc_service_task, "vp_stc", VP_TASK_STACK_LARGE, NULL,
                                VP_TASK_PRIO_NORMAL, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "创建 STC 任务失败");
    ESP_LOGI(TAG, "STC 服务初始化完成，已启用自定义 RTU 帧协议");
    return ESP_OK;
#endif
}
