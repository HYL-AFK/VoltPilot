#ifndef STC_SERVICE_H
#define STC_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool online;
    bool gear_valid;
    uint8_t raw_gear;
    uint8_t protocol_version;
    uint8_t firmware_major;
    uint8_t firmware_minor;
    uint8_t firmware_patch;
    uint8_t hardware_major;
    uint8_t hardware_minor;
    uint32_t uptime_ms;
    uint16_t status_flags;
    uint16_t adc_key_raw;
    uint16_t debounce_ms;
    uint16_t io_inputs;
    uint16_t io_outputs;
    uint32_t rx_frames;
    uint32_t parsed_frames;
    uint32_t crc_error_count;
    uint32_t parse_error_count;
    uint32_t timeout_count;
    uint32_t last_rx_ms;
    uint32_t last_parse_ms;
} stc_info_t;

esp_err_t stc_service_init(void);
bool stc_service_get_info(stc_info_t *out_info);

#endif
