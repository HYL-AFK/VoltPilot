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
#include "runtime_history.h"
#include "stc_service.h"

#define VP_DIAG_FAULT_SOURCE_LEN 16
#define VP_DIAG_TASK_NAME_LEN 12
#define VP_DIAG_WDT_TASK_COUNT 8
#define VP_DIAG_EVENT_AI_RS485_OFFLINE 0xA1000001U
#define VP_DIAG_EVENT_AI_RS485_RECOVERED 0xA1000002U

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
    uint32_t last_fault_ms;
    bool fault_prev_en_24v;
    bool fault_prev_en_36v;
    bool fault_prev_en_48v;

    vp_adc_channel_value_t adc[VP_ADC_INPUT_COUNT];
    uint32_t adc_update_ms;
    bool adc_valid;

    bool bms_online;
    bool bms_parsed_valid;
    uint8_t bms_cell_count;
    int32_t bms_pack_mv;
    int32_t bms_current_ma;
    uint8_t bms_rsoc_percent;
    uint16_t bms_soh_percent;
    uint8_t bms_protect_1;
    uint8_t bms_protect_2;
    uint8_t bms_protect_3;
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
    uint8_t stc_firmware_major;
    uint8_t stc_firmware_minor;
    uint8_t stc_firmware_patch;
    uint8_t stc_hardware_major;
    uint8_t stc_hardware_minor;
    uint16_t stc_io_inputs;
    uint16_t stc_io_outputs;
    uint32_t stc_crc_error_count;
    uint32_t stc_parse_error_count;
    uint32_t stc_timeout_count;

    bool last_boot_wdt;
    vp_diag_watchdog_task_t wdt_tasks[VP_DIAG_WDT_TASK_COUNT];
} vp_diag_registers_t;

esp_err_t diag_service_init(void);
/* 每秒采样运行状态；由应用主循环调用，不依赖任意通信服务。 */
esp_err_t diag_service_tick(void);
esp_err_t diag_service_get_snapshot(vp_diag_registers_t *out_snapshot);
size_t diag_service_runtime_history_count(void);
esp_err_t diag_service_get_runtime_history(size_t index, vp_runtime_history_record_t *out_record);
esp_err_t diag_service_get_runtime_summary(vp_runtime_history_summary_t *out_summary);
esp_err_t diag_service_record_fault(vp_fault_code_t code, const char *source);
/* 记录非故障状态的关键诊断事件，不改变当前 fault_code。 */
esp_err_t diag_service_record_event(uint32_t event_code);
esp_err_t diag_service_capture_fault_outputs(board_output_state_t outputs);
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
