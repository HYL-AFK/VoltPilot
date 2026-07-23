#include "stc_protocol.h"

#include <string.h>

uint16_t stc_protocol_crc16_modbus(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xffff;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if ((crc & 0x0001) != 0) {
                crc = (crc >> 1) ^ 0xa001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

const char *stc_protocol_func_name(uint8_t func)
{
    switch (func & 0x7f) {
    case STC_FUNC_HEARTBEAT:
        return "HEARTBEAT";
    case STC_FUNC_READ_GEAR:
        return "READ_GEAR";
    case STC_FUNC_READ_IO_STATUS:
        return "READ_IO_STATUS";
    case STC_FUNC_WRITE_CONTROL:
        return "WRITE_CONTROL";
    case STC_FUNC_READ_VERSION:
        return "READ_VERSION";
    default:
        return "UNKNOWN";
    }
}

const char *stc_protocol_error_name(uint8_t error_code)
{
    switch (error_code) {
    case STC_ERROR_CRC:
        return "CRC_ERROR";
    case STC_ERROR_LENGTH:
        return "LENGTH_ERROR";
    case STC_ERROR_UNSUPPORTED_FUNC:
        return "UNSUPPORTED_FUNC";
    case STC_ERROR_BUSY:
        return "BUSY";
    default:
        return "UNKNOWN_ERROR";
    }
}

bool stc_protocol_is_exception_func(uint8_t func)
{
    return (func & 0x80) != 0;
}

esp_err_t stc_protocol_build_request(uint8_t addr,
                                     uint8_t func,
                                     uint8_t seq,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint8_t *out_frame,
                                     size_t out_size,
                                     size_t *out_len)
{
    if (out_frame == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_len > STC_PROTOCOL_MAX_PAYLOAD_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (payload_len > 0 && payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t frame_len = STC_PROTOCOL_HEADER_LEN + payload_len + STC_PROTOCOL_CRC_LEN;
    if (out_size < frame_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    out_frame[0] = addr;
    out_frame[1] = func;
    out_frame[2] = seq;
    out_frame[3] = (uint8_t)payload_len;
    if (payload_len > 0) {
        memcpy(out_frame + STC_PROTOCOL_HEADER_LEN, payload, payload_len);
    }

    uint16_t crc = stc_protocol_crc16_modbus(out_frame, STC_PROTOCOL_HEADER_LEN + payload_len);
    out_frame[STC_PROTOCOL_HEADER_LEN + payload_len] = (uint8_t)(crc & 0xff);
    out_frame[STC_PROTOCOL_HEADER_LEN + payload_len + 1] = (uint8_t)(crc >> 8);
    *out_len = frame_len;
    return ESP_OK;
}

esp_err_t stc_protocol_try_parse(const uint8_t *buffer,
                                 size_t buffer_len,
                                 stc_protocol_frame_t *out_frame,
                                 size_t *consumed)
{
    if (buffer == NULL || out_frame == NULL || consumed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *consumed = 0;
    if (buffer_len < STC_PROTOCOL_MIN_FRAME_LEN) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t payload_len = buffer[3];
    if (payload_len > STC_PROTOCOL_MAX_PAYLOAD_LEN) {
        *consumed = 1;
        return ESP_ERR_INVALID_SIZE;
    }

    size_t frame_len = STC_PROTOCOL_HEADER_LEN + payload_len + STC_PROTOCOL_CRC_LEN;
    if (buffer_len < frame_len) {
        return ESP_ERR_TIMEOUT;
    }

    uint16_t crc_calc = stc_protocol_crc16_modbus(buffer, frame_len - STC_PROTOCOL_CRC_LEN);
    uint16_t crc_rx = (uint16_t)buffer[frame_len - 2] | ((uint16_t)buffer[frame_len - 1] << 8);
    if (crc_calc != crc_rx) {
        *consumed = 1;
        out_frame->crc_calc = crc_calc;
        out_frame->crc_rx = crc_rx;
        out_frame->frame_len = frame_len;
        return ESP_ERR_INVALID_CRC;
    }

    out_frame->addr = buffer[0];
    out_frame->func = buffer[1];
    out_frame->seq = buffer[2];
    out_frame->payload_len = payload_len;
    out_frame->payload = buffer + STC_PROTOCOL_HEADER_LEN;
    out_frame->crc_rx = crc_rx;
    out_frame->crc_calc = crc_calc;
    out_frame->frame_len = frame_len;
    *consumed = frame_len;
    return ESP_OK;
}
