#ifndef STC_PROTOCOL_H
#define STC_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define STC_PROTOCOL_HOST_ADDR 0x00
#define STC_PROTOCOL_SLAVE_ADDR 0x11
#define STC_PROTOCOL_BROADCAST_ADDR 0xff

#define STC_PROTOCOL_HEADER_LEN 4
#define STC_PROTOCOL_CRC_LEN 2
#define STC_PROTOCOL_MIN_FRAME_LEN (STC_PROTOCOL_HEADER_LEN + STC_PROTOCOL_CRC_LEN)
#define STC_PROTOCOL_MAX_PAYLOAD_LEN 64
#define STC_PROTOCOL_MAX_FRAME_LEN (STC_PROTOCOL_HEADER_LEN + STC_PROTOCOL_MAX_PAYLOAD_LEN + STC_PROTOCOL_CRC_LEN)

typedef enum {
    STC_FUNC_HEARTBEAT = 0x01,
    STC_FUNC_READ_GEAR = 0x02,
    STC_FUNC_READ_IO_STATUS = 0x03,
    STC_FUNC_WRITE_CONTROL = 0x04,
    STC_FUNC_READ_VERSION = 0x05,
} stc_protocol_func_t;

typedef enum {
    STC_ERROR_CRC = 0x01,
    STC_ERROR_LENGTH = 0x02,
    STC_ERROR_UNSUPPORTED_FUNC = 0x03,
    STC_ERROR_BUSY = 0x04,
} stc_protocol_error_t;

typedef struct {
    uint8_t addr;
    uint8_t func;
    uint8_t seq;
    uint8_t payload_len;
    const uint8_t *payload;
    uint16_t crc_rx;
    uint16_t crc_calc;
    size_t frame_len;
} stc_protocol_frame_t;

uint16_t stc_protocol_crc16_modbus(const uint8_t *data, size_t len);
const char *stc_protocol_func_name(uint8_t func);
const char *stc_protocol_error_name(uint8_t error_code);
bool stc_protocol_is_exception_func(uint8_t func);

esp_err_t stc_protocol_build_request(uint8_t addr,
                                     uint8_t func,
                                     uint8_t seq,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint8_t *out_frame,
                                     size_t out_size,
                                     size_t *out_len);

esp_err_t stc_protocol_try_parse(const uint8_t *buffer,
                                 size_t buffer_len,
                                 stc_protocol_frame_t *out_frame,
                                 size_t *consumed);

#endif
