#include "ai_rs485_service.h"

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "vp_board.h"

static const char *TAG = "ai_rs485";
static ai_rs485_snapshot_t s_snapshot;

/* 模块当前使用固定两位小数格式：raw=534 表示 5.34V。 */
static uint16_t raw_to_rounded_voltage_v(uint16_t raw)
{
    uint16_t integer = raw / 100;
    uint16_t tenths = (raw % 100) / 10;

    /* 四舍六入五成双：0~4 舍去，6~9 进位，5 时保留为偶数。 */
    if (tenths > 5 || (tenths == 5 && (integer & 1U) != 0U)) {
        integer++;
    }
    return integer;
}

/* Modbus RTU CRC16，低字节先发送。 */
static uint16_t crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
        }
    }
    return crc;
}

static void poll_task(void *arg)
{
    (void)arg;
    /* 读取 4 路输入寄存器：地址 0x0000，功能码 0x04。 */
    uint8_t request[] = {VP_AI_RS485_DEVICE_ADDR, 0x04, 0x00, 0x00, 0x00,
                         VP_AI_RS485_CHANNEL_COUNT, 0x00, 0x00};
    uint16_t crc = crc16(request, 6);
    request[6] = (uint8_t)crc;
    request[7] = (uint8_t)(crc >> 8);
    uint8_t response[32];

    while (true) {
        /* 当前模块配置为普通问答模式，不启用主动上传。 */
        uart_flush_input(VP_AI_RS485_UART_PORT);
        (void)uart_write_bytes(VP_AI_RS485_UART_PORT, request, sizeof(request));
        int len = uart_read_bytes(VP_AI_RS485_UART_PORT, response, sizeof(response),
                                  pdMS_TO_TICKS(200));
        if (len >= 5 && response[0] == VP_AI_RS485_DEVICE_ADDR && response[1] == 0x04) {
            uint8_t byte_count = response[2];
            size_t frame_len = 3 + byte_count + 2;
            /* 先校验长度，再访问 CRC 字段，防止异常帧造成越界。 */
            if (byte_count == 0 || byte_count > VP_AI_RS485_CHANNEL_COUNT * 2 ||
                frame_len > (size_t)len) {
                s_snapshot.crc_errors++;
            } else {
                uint16_t received = (uint16_t)response[frame_len - 2] |
                                    ((uint16_t)response[frame_len - 1] << 8);
                if (crc16(response, frame_len - 2) != received) {
                    s_snapshot.crc_errors++;
                    goto poll_delay;
                }
                uint8_t channels = byte_count / 2;
                if (channels > VP_AI_RS485_CHANNEL_COUNT) channels = VP_AI_RS485_CHANNEL_COUNT;
                for (uint8_t i = 0; i < channels; i++) {
                    s_snapshot.raw[i] = ((uint16_t)response[3 + i * 2] << 8) |
                                        response[4 + i * 2];
                }
                s_snapshot.online = true;
                s_snapshot.rx_frames++;
                ESP_LOGI(TAG, "AI RX channels=%u raw0=%u raw1=%u raw2=%u raw3=%u | V0=%uV V1=%uV V2=%uV V3=%uV",
                         channels, s_snapshot.raw[0], s_snapshot.raw[1],
                         s_snapshot.raw[2], s_snapshot.raw[3],
                         raw_to_rounded_voltage_v(s_snapshot.raw[0]),
                         raw_to_rounded_voltage_v(s_snapshot.raw[1]),
                         raw_to_rounded_voltage_v(s_snapshot.raw[2]),
                         raw_to_rounded_voltage_v(s_snapshot.raw[3]));
            }
        } else {
            s_snapshot.online = false;
            s_snapshot.timeout_count++;
            ESP_LOGW(TAG, "AI RS485 timeout count=%u", (unsigned)s_snapshot.timeout_count);
        }
poll_delay:
        vTaskDelay(pdMS_TO_TICKS(VP_AI_RS485_POLL_INTERVAL_MS));
    }
}

bool ai_rs485_service_get_snapshot(ai_rs485_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) return false;
    *out_snapshot = s_snapshot;
    return s_snapshot.online;
}

esp_err_t ai_rs485_service_init(void)
{
    /* HW-519 类收发器负责 UART-TTL 与 RS485 差分信号转换。 */
    uart_config_t cfg = {
        .baud_rate = VP_AI_RS485_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(VP_AI_RS485_UART_PORT, 512, 256, 0, NULL, 0),
                        TAG, "AI UART install failed");
    ESP_RETURN_ON_ERROR(uart_param_config(VP_AI_RS485_UART_PORT, &cfg), TAG, "AI UART config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(VP_AI_RS485_UART_PORT, VP_AI_RS485_PIN_TX,
                                     VP_AI_RS485_PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "AI UART pins failed");
    BaseType_t ok = xTaskCreate(poll_task, "vp_ai_rs485", VP_TASK_STACK_LARGE, NULL,
                                VP_TASK_PRIO_NORMAL, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "AI task create failed");
    ESP_LOGI(TAG, "AI RS485 initialized addr=%d baud=%d", VP_AI_RS485_DEVICE_ADDR,
             VP_AI_RS485_BAUD_RATE);
    return ESP_OK;
}
