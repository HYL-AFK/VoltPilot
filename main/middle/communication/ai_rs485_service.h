#ifndef AI_RS485_SERVICE_H
#define AI_RS485_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool online;
    uint16_t raw[4];
    uint32_t rx_frames;
    uint32_t crc_errors;
    uint32_t timeout_count;
    uint8_t consecutive_failures;
} ai_rs485_snapshot_t;

/* 初始化 AI 模拟量模块 Modbus RTU 轮询服务。 */
esp_err_t ai_rs485_service_init(void);
/* 获取最近一次采样快照；返回值表示最近一次通信是否有效。 */
bool ai_rs485_service_get_snapshot(ai_rs485_snapshot_t *out_snapshot);
/* 返回最近一次完整四通道响应是否有效。 */
bool ai_rs485_service_is_ready(void);

#endif
