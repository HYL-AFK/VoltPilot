#include "diag_service.h"

#include <inttypes.h>
#include <stddef.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DIAG_MAGIC 0x56504447U
#define DIAG_VERSION 1U
#define DIAG_NAMESPACE "vp_diag"
#define DIAG_KEY_A "diag_a"
#define DIAG_KEY_B "diag_b"
#define DIAG_BG_SAVE_INTERVAL_MS 60000U
#define DIAG_ERROR_SAVE_INTERVAL_MS 5000U

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t seq;
    uint32_t crc32;
    vp_diag_registers_t regs;
} diag_slot_t;

static const char *TAG = "diag_service";

static nvs_handle_t s_nvs;
static bool s_inited;
static uint32_t s_seq;
static uint32_t s_last_bg_save_ms;
static uint32_t s_last_error_save_ms;
static vp_diag_registers_t s_regs;

static uint32_t now_ms(void)
{
    return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

static uint32_t slot_crc(const diag_slot_t *slot)
{
    diag_slot_t tmp = *slot;
    tmp.crc32 = 0;
    return crc32_update(0, (const uint8_t *)&tmp, sizeof(tmp));
}

static bool slot_is_valid(const diag_slot_t *slot)
{
    return slot->magic == DIAG_MAGIC &&
           slot->version == DIAG_VERSION &&
           slot->crc32 == slot_crc(slot);
}

static esp_err_t load_slot(const char *key, diag_slot_t *slot, bool *valid)
{
    size_t len = sizeof(*slot);
    esp_err_t err = nvs_get_blob(s_nvs, key, slot, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *valid = false;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "read diag slot failed");
    *valid = len == sizeof(*slot) && slot_is_valid(slot);
    return ESP_OK;
}

static esp_err_t save_snapshot(bool force)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t tick_ms = now_ms();
    if (!force && (tick_ms - s_last_bg_save_ms) < DIAG_BG_SAVE_INTERVAL_MS) {
        return ESP_OK;
    }

    s_regs.min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    diag_slot_t slot = {
        .magic = DIAG_MAGIC,
        .version = DIAG_VERSION,
        .seq = ++s_seq,
        .regs = s_regs,
    };
    slot.crc32 = slot_crc(&slot);

    const char *key = (slot.seq & 1U) ? DIAG_KEY_A : DIAG_KEY_B;
    ESP_RETURN_ON_ERROR(nvs_set_blob(s_nvs, key, &slot, sizeof(slot)), TAG, "write diag slot failed");
    ESP_RETURN_ON_ERROR(nvs_commit(s_nvs), TAG, "commit diag slot failed");
    s_last_bg_save_ms = tick_ms;
    ESP_LOGI(TAG, "诊断寄存器已保存 key=%s seq=%" PRIu32, key, slot.seq);
    return ESP_OK;
}

static esp_err_t save_error_snapshot(void)
{
    uint32_t tick_ms = now_ms();
    if ((tick_ms - s_last_error_save_ms) < DIAG_ERROR_SAVE_INTERVAL_MS) {
        return ESP_OK;
    }
    s_last_error_save_ms = tick_ms;
    return save_snapshot(true);
}

static bool reset_reason_is_wdt(esp_reset_reason_t reason)
{
    return reason == ESP_RST_INT_WDT ||
           reason == ESP_RST_TASK_WDT ||
           reason == ESP_RST_WDT;
}

esp_err_t diag_service_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase nvs failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init failed");
    ESP_RETURN_ON_ERROR(nvs_open(DIAG_NAMESPACE, NVS_READWRITE, &s_nvs), TAG, "open diag nvs failed");

    diag_slot_t a = {0};
    diag_slot_t b = {0};
    bool a_valid = false;
    bool b_valid = false;
    ESP_RETURN_ON_ERROR(load_slot(DIAG_KEY_A, &a, &a_valid), TAG, "load diag A failed");
    ESP_RETURN_ON_ERROR(load_slot(DIAG_KEY_B, &b, &b_valid), TAG, "load diag B failed");

    const diag_slot_t *latest = NULL;
    if (a_valid && b_valid) {
        latest = a.seq >= b.seq ? &a : &b;
    } else if (a_valid) {
        latest = &a;
    } else if (b_valid) {
        latest = &b;
    }

    if (latest != NULL) {
        s_regs = latest->regs;
        s_seq = latest->seq;
    } else {
        memset(&s_regs, 0, sizeof(s_regs));
        s_seq = 0;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    esp_reset_reason_t reason = esp_reset_reason();
    s_regs.version = DIAG_VERSION;
    s_regs.boot_count++;
    s_regs.reset_reason = (uint32_t)reason;
    s_regs.last_boot_wdt = reset_reason_is_wdt(reason);
    strlcpy(s_regs.app_version, app != NULL ? app->version : "unknown", sizeof(s_regs.app_version));
    s_regs.min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);

    s_inited = true;
    ESP_LOGI(TAG, "诊断寄存器初始化 boot=%" PRIu32 " reset=%d last_wdt=%d prev_valid=%d",
             s_regs.boot_count, reason, s_regs.last_boot_wdt, latest != NULL);
    return save_snapshot(true);
}

esp_err_t diag_service_get_snapshot(vp_diag_registers_t *out_snapshot)
{
    ESP_RETURN_ON_FALSE(out_snapshot != NULL, ESP_ERR_INVALID_ARG, TAG, "snapshot null");
    *out_snapshot = s_regs;
    return ESP_OK;
}

esp_err_t diag_service_record_fault(vp_fault_code_t code, const char *source)
{
    if (code != VP_FAULT_NONE) {
        s_regs.last_fault_ms = now_ms();
    }
    s_regs.fault_code = (uint8_t)code;
    strlcpy(s_regs.fault_source, source != NULL ? source : "unknown", sizeof(s_regs.fault_source));
    return save_snapshot(true);
}

esp_err_t diag_service_capture_fault_outputs(board_output_state_t outputs)
{
    s_regs.fault_prev_en_24v = outputs.en_24v;
    s_regs.fault_prev_en_36v = outputs.en_36v;
    s_regs.fault_prev_en_48v = outputs.en_48v;
    return ESP_OK;
}

esp_err_t diag_service_update_state(vp_app_state_t state,
                                    vp_fault_code_t fault,
                                    const char *fault_source,
                                    board_output_state_t outputs)
{
    s_regs.app_state = (uint8_t)state;
    s_regs.fault_code = (uint8_t)fault;
    strlcpy(s_regs.fault_source, fault_source != NULL ? fault_source : "none", sizeof(s_regs.fault_source));
    s_regs.en_24v = outputs.en_24v;
    s_regs.en_36v = outputs.en_36v;
    s_regs.en_48v = outputs.en_48v;
    return save_snapshot(fault != VP_FAULT_NONE);
}

esp_err_t diag_service_update_outputs(board_output_state_t outputs)
{
    s_regs.en_24v = outputs.en_24v;
    s_regs.en_36v = outputs.en_36v;
    s_regs.en_48v = outputs.en_48v;
    return save_snapshot(true);
}

esp_err_t diag_service_update_adc(const vp_adc_snapshot_t *snapshot)
{
    ESP_RETURN_ON_FALSE(snapshot != NULL, ESP_ERR_INVALID_ARG, TAG, "adc snapshot null");
    memcpy(s_regs.adc, snapshot->channel, sizeof(s_regs.adc));
    s_regs.adc_update_ms = snapshot->update_ms;
    s_regs.adc_valid = snapshot->valid;
    return save_snapshot(false);
}

esp_err_t diag_service_update_bms(const bms_info_t *info)
{
    ESP_RETURN_ON_FALSE(info != NULL, ESP_ERR_INVALID_ARG, TAG, "bms info null");
    bool error_changed = info->crc_error_count != s_regs.bms_crc_error_count ||
                         info->parse_error_count != s_regs.bms_parse_error_count ||
                         info->timeout_count != s_regs.bms_timeout_count;

    s_regs.bms_online = info->online;
    s_regs.bms_parsed_valid = info->parsed_valid;
    s_regs.bms_cell_count = info->cell_count;
    s_regs.bms_pack_mv = info->pack_mv;
    s_regs.bms_current_ma = info->current_ma;
    s_regs.bms_rsoc_percent = info->rsoc_percent;
    s_regs.bms_soh_percent = info->soh_percent;
    s_regs.bms_protect_1 = info->protect_1;
    s_regs.bms_protect_2 = info->protect_2;
    s_regs.bms_protect_3 = info->protect_3;
    s_regs.bms_crc_error_count = info->crc_error_count;
    s_regs.bms_parse_error_count = info->parse_error_count;
    s_regs.bms_timeout_count = info->timeout_count;
    s_regs.bms_last_crc_calc = info->last_crc_calc;
    s_regs.bms_last_crc_rx_le = info->last_crc_rx_le;
    s_regs.bms_last_crc_rx_be = info->last_crc_rx_be;

    return error_changed ? save_error_snapshot() : save_snapshot(false);
}

esp_err_t diag_service_update_stc(const stc_info_t *info)
{
    ESP_RETURN_ON_FALSE(info != NULL, ESP_ERR_INVALID_ARG, TAG, "stc info null");
    bool error_changed = info->crc_error_count != s_regs.stc_crc_error_count ||
                         info->parse_error_count != s_regs.stc_parse_error_count ||
                         info->timeout_count != s_regs.stc_timeout_count;

    s_regs.stc_online = info->online;
    s_regs.stc_gear_valid = info->gear_valid;
    s_regs.stc_raw_gear = info->raw_gear;
    s_regs.stc_protocol_version = info->protocol_version;
    s_regs.stc_firmware_major = info->firmware_major;
    s_regs.stc_firmware_minor = info->firmware_minor;
    s_regs.stc_firmware_patch = info->firmware_patch;
    s_regs.stc_hardware_major = info->hardware_major;
    s_regs.stc_hardware_minor = info->hardware_minor;
    s_regs.stc_io_inputs = info->io_inputs;
    s_regs.stc_io_outputs = info->io_outputs;
    s_regs.stc_crc_error_count = info->crc_error_count;
    s_regs.stc_parse_error_count = info->parse_error_count;
    s_regs.stc_timeout_count = info->timeout_count;

    return error_changed ? save_error_snapshot() : save_snapshot(false);
}

esp_err_t diag_service_update_watchdog_task(const char *task_name, uint32_t feed_ms)
{
    if (task_name == NULL || task_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    int free_slot = -1;
    for (int i = 0; i < VP_DIAG_WDT_TASK_COUNT; i++) {
        if (strncmp(s_regs.wdt_tasks[i].name, task_name, sizeof(s_regs.wdt_tasks[i].name)) == 0) {
            s_regs.wdt_tasks[i].last_feed_ms = feed_ms;
            return ESP_OK;
        }
        if (free_slot < 0 && s_regs.wdt_tasks[i].name[0] == '\0') {
            free_slot = i;
        }
    }

    if (free_slot >= 0) {
        strlcpy(s_regs.wdt_tasks[free_slot].name, task_name, sizeof(s_regs.wdt_tasks[free_slot].name));
        s_regs.wdt_tasks[free_slot].last_feed_ms = feed_ms;
    }
    return ESP_OK;
}
