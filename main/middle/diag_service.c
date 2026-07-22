#include "diag_service.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "ai_rs485_service.h"
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
#define RUNTIME_NAMESPACE "vp_runtime"
#define RUNTIME_META_KEY "meta"
#define RUNTIME_RECORD_COUNT 32U
#define RUNTIME_RECORD_INTERVAL_MS 60000U
#define RUNTIME_MAGIC 0x56505248U
#define RUNTIME_VERSION 1U

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    uint16_t capacity;
    uint16_t head;
    uint16_t count;
    uint16_t reserved;
    uint32_t next_sequence;
    uint32_t crc32;
} runtime_meta_t;

typedef struct __attribute__((packed)) {
    uint32_t sequence;
    uint32_t uptime_ms;
    uint32_t timestamp;
    uint32_t boot_count;
    uint8_t app_state;
    uint8_t fault_code;
    uint8_t gear;
    uint8_t gear_valid;
    uint8_t stc_online;
    uint8_t bms_online;
    uint8_t ai_online;
    uint8_t en_state;
    uint16_t stc_io_inputs;
    uint16_t stc_io_outputs;
    uint16_t ai_raw[4];
    uint16_t adc_mv[4];
    uint32_t stc_timeout_count;
    uint32_t bms_timeout_count;
    uint32_t ai_timeout_count;
    uint8_t reserved[4];
    uint32_t crc32;
} runtime_record_t;

_Static_assert(sizeof(runtime_record_t) == 64, "runtime record must be 64 bytes");

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
static nvs_handle_t s_runtime_nvs;
static runtime_meta_t s_runtime_meta;
static uint32_t s_last_runtime_save_ms;
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

static uint32_t runtime_meta_crc(const runtime_meta_t *meta)
{
    runtime_meta_t copy = *meta;
    copy.crc32 = 0;
    return crc32_update(0, (const uint8_t *)&copy, sizeof(copy));
}

static uint32_t runtime_record_crc(const runtime_record_t *record)
{
    runtime_record_t copy = *record;
    copy.crc32 = 0;
    return crc32_update(0, (const uint8_t *)&copy, sizeof(copy));
}

static void runtime_record_key(uint16_t index, char *key, size_t key_size)
{
    (void)snprintf(key, key_size, "r%02u", (unsigned)index);
}

static esp_err_t save_runtime_meta(void)
{
    s_runtime_meta.crc32 = runtime_meta_crc(&s_runtime_meta);
    ESP_RETURN_ON_ERROR(nvs_set_blob(s_runtime_nvs, RUNTIME_META_KEY,
                                     &s_runtime_meta, sizeof(s_runtime_meta)),
                        TAG, "write runtime meta failed");
    return nvs_commit(s_runtime_nvs);
}

static esp_err_t save_runtime_record(uint32_t tick_ms)
{
    if ((tick_ms - s_last_runtime_save_ms) < RUNTIME_RECORD_INTERVAL_MS &&
        s_runtime_meta.next_sequence != 0) {
        return ESP_OK;
    }

    ai_rs485_snapshot_t ai = {0};
    (void)ai_rs485_service_get_snapshot(&ai);
    time_t wall_time = time(NULL);

    runtime_record_t record = {
        .sequence = ++s_runtime_meta.next_sequence,
        .uptime_ms = tick_ms,
        .timestamp = wall_time >= 1577836800 ? (uint32_t)wall_time : 0,
        .boot_count = s_regs.boot_count,
        .app_state = s_regs.app_state,
        .fault_code = s_regs.fault_code,
        .gear = s_regs.stc_raw_gear,
        .gear_valid = s_regs.stc_gear_valid,
        .stc_online = s_regs.stc_online,
        .bms_online = s_regs.bms_online,
        .ai_online = ai.online,
        .en_state = (uint8_t)((s_regs.en_24v ? 1U : 0U) |
                              (s_regs.en_36v ? 2U : 0U) |
                              (s_regs.en_48v ? 4U : 0U)),
        .stc_io_inputs = s_regs.stc_io_inputs,
        .stc_io_outputs = s_regs.stc_io_outputs,
        .stc_timeout_count = s_regs.stc_timeout_count,
        .bms_timeout_count = s_regs.bms_timeout_count,
        .ai_timeout_count = ai.timeout_count,
    };

    for (size_t i = 0; i < 4; i++) {
        record.ai_raw[i] = ai.raw[i];
        record.adc_mv[i] = (uint16_t)(s_regs.adc[i].bus_mv > UINT16_MAX ?
                                      UINT16_MAX : s_regs.adc[i].bus_mv);
    }

    char key[8];
    runtime_record_key(s_runtime_meta.head, key, sizeof(key));
    record.crc32 = runtime_record_crc(&record);
    ESP_RETURN_ON_ERROR(nvs_set_blob(s_runtime_nvs, key, &record, sizeof(record)),
                        TAG, "write runtime record failed");

    s_runtime_meta.head = (uint16_t)((s_runtime_meta.head + 1U) % RUNTIME_RECORD_COUNT);
    if (s_runtime_meta.count < RUNTIME_RECORD_COUNT) {
        s_runtime_meta.count++;
    }
    s_last_runtime_save_ms = tick_ms;
    return save_runtime_meta();
}

static esp_err_t init_runtime_history(void)
{
    esp_err_t err = nvs_open(RUNTIME_NAMESPACE, NVS_READWRITE, &s_runtime_nvs);
    ESP_RETURN_ON_ERROR(err, TAG, "open runtime history failed");

    size_t len = sizeof(s_runtime_meta);
    err = nvs_get_blob(s_runtime_nvs, RUNTIME_META_KEY, &s_runtime_meta, &len);
    if (err != ESP_OK || len != sizeof(s_runtime_meta) ||
        s_runtime_meta.magic != RUNTIME_MAGIC ||
        s_runtime_meta.version != RUNTIME_VERSION ||
        s_runtime_meta.record_size != sizeof(runtime_record_t) ||
        s_runtime_meta.capacity != RUNTIME_RECORD_COUNT ||
        s_runtime_meta.crc32 != runtime_meta_crc(&s_runtime_meta)) {
        memset(&s_runtime_meta, 0, sizeof(s_runtime_meta));
        s_runtime_meta.magic = RUNTIME_MAGIC;
        s_runtime_meta.version = RUNTIME_VERSION;
        s_runtime_meta.record_size = sizeof(runtime_record_t);
        s_runtime_meta.capacity = RUNTIME_RECORD_COUNT;
    }
    return ESP_OK;
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
    ESP_RETURN_ON_ERROR(save_runtime_record(tick_ms), TAG, "save runtime history failed");
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
    ESP_RETURN_ON_ERROR(init_runtime_history(), TAG, "init runtime history failed");

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
