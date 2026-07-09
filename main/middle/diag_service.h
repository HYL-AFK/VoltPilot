#ifndef DIAG_SERVICE_H
#define DIAG_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "adc_service.h"
#include "app_state_service.h"
#include "bms_service.h"
#include "board_service.h"
#include "fault_service.h"
#include "stc_service.h"

#define VP_DIAG_FAULT_SOURCE_LEN 16
#define VP_DIAG_TASK_NAME_LEN 12
#define VP_DIAG_WDT_TASK_COUNT 6

typedef struct {
    char name[VP_DIAG_TASK_NAME_LEN];
    uint32_t last_feed_ms;
} vp_diag_watchdog_task_t;

typedef struct {
    uint32_t version;
    uint32_t boot_count;
    uint32_t reset_reason;
    uint32_t min_free_heap;
    char app_version[16];

    uint8_t app_state;
    uint8_t fault_code;
    char fault_source[VP_DIAG_FAULT_SOURCE_LEN];
    bool en_24v;
    bool en_36v;
    bool en_48v;

    vp_adc_channel_value_t adc[VP_ADC_INPUT_COUNT];
    uint32_t adc_update_ms;
    bool adc_valid;

    bool bms_online;
    bool bms_parsed_valid;
    uint8_t bms_cell_count;
    int32_t bms_pack_mv;
    int32_t bms_current_ma;
    uint8_t bms_rsoc_percent;
    uint32_t bms_crc_error_count;
    uint32_t bms_parse_error_count;
    uint32_t bms_timeout_count;
    uint16_t bms_last_crc_calc;
    uint16_t bms_last_crc_rx_le;
    uint16_t bms_last_crc_rx_be;

    bool stc_online;
    bool stc_gear_valid;
    uint8_t stc_raw_gear;
    uint8_t stc_protocol_version;
    uint32_t stc_crc_error_count;
    uint32_t stc_parse_error_count;
    uint32_t stc_timeout_count;

    bool last_boot_wdt;
    vp_diag_watchdog_task_t wdt_tasks[VP_DIAG_WDT_TASK_COUNT];
} vp_diag_registers_t;

esp_err_t diag_service_init(void);
esp_err_t diag_service_get_snapshot(vp_diag_registers_t *out_snapshot);
esp_err_t diag_service_record_fault(vp_fault_code_t code, const char *source);
esp_err_t diag_service_update_state(vp_app_state_t state,
                                    vp_fault_code_t fault,
                                    const char *fault_source,
                                    board_output_state_t outputs);
esp_err_t diag_service_update_outputs(board_output_state_t outputs);
esp_err_t diag_service_update_adc(const vp_adc_snapshot_t *snapshot);
esp_err_t diag_service_update_bms(const bms_info_t *info);
esp_err_t diag_service_update_stc(const stc_info_t *info);
esp_err_t diag_service_update_watchdog_task(const char *task_name, uint32_t feed_ms);

#endif

