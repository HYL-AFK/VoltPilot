#include "bms_protocol.h"

#include <stdbool.h>
#include <string.h>

#include "esp_err.h"

enum {
    BMS_BODY_OFF_ID = 0,
    BMS_BODY_OFF_SOFT_ID = BMS_BODY_OFF_ID + BMS_ID_TEXT_LEN,
    BMS_BODY_OFF_PACK_MV = BMS_BODY_OFF_SOFT_ID + BMS_SOFT_ID_TEXT_LEN,
    BMS_BODY_OFF_CURRENT_MA = BMS_BODY_OFF_PACK_MV + 4,
    BMS_BODY_OFF_CELL_MV = BMS_BODY_OFF_CURRENT_MA + 4,
};

static uint16_t read_be16(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8) | data[1];
}

static int16_t read_be16_signed(const uint8_t *data)
{
    return (int16_t)read_be16(data);
}

static int32_t read_be32_signed(const uint8_t *data)
{
    uint32_t value = ((uint32_t)data[0] << 24) |
                     ((uint32_t)data[1] << 16) |
                     ((uint32_t)data[2] << 8) |
                     data[3];
    return (int32_t)value;
}

static void copy_ascii_field(char *dst, size_t dst_size, const uint8_t *src, size_t src_len)
{
    size_t copy_len = src_len < dst_size - 1 ? src_len : dst_size - 1;

    for (size_t i = 0; i < copy_len; i++) {
        uint8_t ch = src[i];
        dst[i] = (ch >= 0x20 && ch <= 0x7e) ? (char)ch : '.';
    }
    dst[copy_len] = '\0';
}

static bms_material_t material_from_soft_id(const uint8_t *soft_id)
{
    if ((soft_id[0] == '0' && soft_id[1] == '1') || soft_id[0] == 0x01) {
        return BMS_MATERIAL_TERNARY_14S;
    }
    if ((soft_id[0] == '0' && soft_id[1] == '2') || soft_id[0] == 0x02) {
        return BMS_MATERIAL_LFP_15S;
    }
    return BMS_MATERIAL_UNKNOWN;
}

const char *bms_material_name(bms_material_t material)
{
    switch (material) {
    case BMS_MATERIAL_TERNARY_14S:
        return "TERNARY_14S";
    case BMS_MATERIAL_LFP_15S:
        return "LFP_15S";
    case BMS_MATERIAL_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

uint16_t bms_protocol_crc16_modbus(const uint8_t *data, size_t len)
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

size_t bms_protocol_expected_len_from_header(const uint8_t *frame, size_t len)
{
    if (frame == NULL || len < BMS_PROTOCOL_HEADER_LEN + BMS_BODY_OFF_SOFT_ID + BMS_SOFT_ID_TEXT_LEN) {
        return 0;
    }
    if (memcmp(frame, BMS_PROTOCOL_HEADER_TEXT, BMS_PROTOCOL_HEADER_LEN) != 0) {
        return 0;
    }

    const uint8_t *soft_id = frame + BMS_PROTOCOL_HEADER_LEN + BMS_BODY_OFF_SOFT_ID;
    switch (material_from_soft_id(soft_id)) {
    case BMS_MATERIAL_TERNARY_14S:
        return BMS_PROTOCOL_FRAME_LEN_14S;
    case BMS_MATERIAL_LFP_15S:
        return BMS_PROTOCOL_FRAME_LEN_15S;
    case BMS_MATERIAL_UNKNOWN:
    default:
        return 0;
    }
}

static bool crc_is_valid(const uint8_t *frame, size_t len, bms_info_t *out_info)
{
    uint16_t calc = bms_protocol_crc16_modbus(frame, len - 2);
    uint16_t calc_body = bms_protocol_crc16_modbus(frame + BMS_PROTOCOL_HEADER_LEN,
                                                   len - BMS_PROTOCOL_HEADER_LEN - 2);
    uint16_t rx_le = (uint16_t)frame[len - 2] | ((uint16_t)frame[len - 1] << 8);
    uint16_t rx_be = ((uint16_t)frame[len - 2] << 8) | frame[len - 1];

    if (out_info != NULL) {
        out_info->last_crc_calc = calc == rx_le || calc == rx_be ? calc : calc_body;
        out_info->last_crc_rx_le = rx_le;
        out_info->last_crc_rx_be = rx_be;
    }

    return calc == rx_le || calc == rx_be || calc_body == rx_le || calc_body == rx_be;
}

esp_err_t bms_protocol_parse(const uint8_t *frame, size_t len, bms_info_t *out_info)
{
    if (frame == NULL || out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len != BMS_PROTOCOL_FRAME_LEN_14S && len != BMS_PROTOCOL_FRAME_LEN_15S) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(frame, BMS_PROTOCOL_HEADER_TEXT, BMS_PROTOCOL_HEADER_LEN) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!crc_is_valid(frame, len, out_info)) {
        return ESP_ERR_INVALID_CRC;
    }

    const uint8_t *body = frame + BMS_PROTOCOL_HEADER_LEN;
    size_t body_len = len - BMS_PROTOCOL_HEADER_LEN - 2;
    uint8_t cell_count = len == BMS_PROTOCOL_FRAME_LEN_15S ? 15 : 14;
    bms_material_t material = material_from_soft_id(body + BMS_BODY_OFF_SOFT_ID);

    if (material == BMS_MATERIAL_TERNARY_14S && len != BMS_PROTOCOL_FRAME_LEN_14S) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (material == BMS_MATERIAL_LFP_15S && len != BMS_PROTOCOL_FRAME_LEN_15S) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (material == BMS_MATERIAL_UNKNOWN) {
        material = len == BMS_PROTOCOL_FRAME_LEN_15S ? BMS_MATERIAL_LFP_15S : BMS_MATERIAL_TERNARY_14S;
    }

    size_t offset = BMS_BODY_OFF_CELL_MV;
    size_t cells_bytes = (size_t)cell_count * 2;
    size_t tail_min = cells_bytes + (BMS_TEMP_COUNT * 2) + 1 + 1 + 2 + 2 + 2 + 2 +
                      1 + 1 + 1 + 1 + 1 + 1 + BMS_BATTERY_CODE_TEXT_LEN +
                      BMS_GLOBAL_ASSET_TEXT_LEN + 1 + 1;
    if (offset + tail_min > body_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(out_info->cell_mv, 0, sizeof(out_info->cell_mv));
    memset(out_info->temp_c, 0, sizeof(out_info->temp_c));
    out_info->material = material;
    out_info->cell_count = cell_count;
    copy_ascii_field(out_info->bms_id, sizeof(out_info->bms_id), body + BMS_BODY_OFF_ID, BMS_ID_TEXT_LEN);
    copy_ascii_field(out_info->soft_id, sizeof(out_info->soft_id), body + BMS_BODY_OFF_SOFT_ID, BMS_SOFT_ID_TEXT_LEN);
    out_info->pack_mv = read_be32_signed(body + BMS_BODY_OFF_PACK_MV);
    out_info->current_ma = read_be32_signed(body + BMS_BODY_OFF_CURRENT_MA);

    for (uint8_t i = 0; i < cell_count; i++) {
        out_info->cell_mv[i] = read_be16(body + offset);
        offset += 2;
    }
    for (uint8_t i = 0; i < BMS_TEMP_COUNT; i++) {
        out_info->temp_c[i] = read_be16_signed(body + offset);
        offset += 2;
    }

    out_info->rsoc_percent = body[offset++];
    out_info->asoc_percent = body[offset++];
    out_info->soc_permille = (int32_t)out_info->rsoc_percent * 10;
    out_info->remaining_capacity_mah = read_be16(body + offset);
    offset += 2;
    out_info->full_charge_capacity_mah = read_be16(body + offset);
    offset += 2;
    out_info->cycle_count = read_be16(body + offset);
    offset += 2;
    out_info->soh_percent = read_be16(body + offset);
    offset += 2;
    out_info->battery_status = body[offset++];
    out_info->charge_mos_state = body[offset++];
    out_info->discharge_mos_state = body[offset++];
    out_info->protect_1 = body[offset++];
    out_info->protect_2 = body[offset++];
    out_info->protect_3 = body[offset++];
    copy_ascii_field(out_info->battery_code, sizeof(out_info->battery_code), body + offset, BMS_BATTERY_CODE_TEXT_LEN);
    offset += BMS_BATTERY_CODE_TEXT_LEN;
    copy_ascii_field(out_info->global_asset_number, sizeof(out_info->global_asset_number),
                     body + offset, BMS_GLOBAL_ASSET_TEXT_LEN);
    offset += BMS_GLOBAL_ASSET_TEXT_LEN;
    out_info->work_mode = body[offset++];
    out_info->predischarge_mos_state = body[offset++];

    return offset == body_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
