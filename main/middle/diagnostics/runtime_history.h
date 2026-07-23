#ifndef RUNTIME_HISTORY_H
#define RUNTIME_HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 1 秒 RAM 队列保留最近 5 分钟，用于故障前后的快速诊断。 */
#define VP_RUNTIME_HISTORY_RAM_CAPACITY 300U
#define VP_RUNTIME_HISTORY_FLASH_INTERVAL_MS 5000U
#define VP_RUNTIME_HISTORY_SUMMARY_INTERVAL_MS 300000U

typedef enum {
    VP_RUNTIME_HISTORY_RECORD_SNAPSHOT = 0,
    VP_RUNTIME_HISTORY_RECORD_EVENT,
    VP_RUNTIME_HISTORY_RECORD_SUMMARY,
} vp_runtime_history_record_type_t;

/*
 * 与具体业务无关的运行状态记录。
 * 其他项目可复用公共字段，并把本项目专有状态放入 user_data。
 */
typedef struct {
    uint32_t sequence;
    uint32_t uptime_ms;
    uint32_t timestamp;
    uint32_t event_code;
    int32_t pack_mv;
    int32_t current_ma;
    uint16_t status_flags;
    uint8_t record_type;
    uint8_t app_state;
    uint8_t fault_code;
    uint8_t gear;
    uint8_t soc_percent;
    int8_t max_temperature_c;
    uint8_t user_data[16];
} vp_runtime_history_record_t;

typedef struct {
    uint32_t window_start_ms;
    uint32_t window_end_ms;
    uint32_t sample_count;
    int32_t min_pack_mv;
    int32_t max_pack_mv;
    int8_t max_temperature_c;
    uint8_t min_soc_percent;
    uint32_t event_count;
} vp_runtime_history_summary_t;

typedef struct {
    vp_runtime_history_record_t ram_records[VP_RUNTIME_HISTORY_RAM_CAPACITY];
    uint16_t ram_head;
    uint16_t ram_count;
    bool persisted_once;
    uint32_t last_persist_ms;
    bool summary_active;
    vp_runtime_history_summary_t summary;
} vp_runtime_history_t;

void vp_runtime_history_init(vp_runtime_history_t *history);
bool vp_runtime_history_push_ram(vp_runtime_history_t *history,
                                 const vp_runtime_history_record_t *record);
size_t vp_runtime_history_ram_count(const vp_runtime_history_t *history);
bool vp_runtime_history_get_ram(const vp_runtime_history_t *history, size_t index,
                                vp_runtime_history_record_t *out_record);
bool vp_runtime_history_get_latest(const vp_runtime_history_t *history,
                                   vp_runtime_history_record_t *out_record);

bool vp_runtime_history_should_persist(const vp_runtime_history_t *history,
                                       const vp_runtime_history_record_t *record,
                                       bool force_event);
void vp_runtime_history_mark_persisted(vp_runtime_history_t *history,
                                       const vp_runtime_history_record_t *record);

void vp_runtime_history_observe(vp_runtime_history_t *history,
                                const vp_runtime_history_record_t *record);
bool vp_runtime_history_summary_due(const vp_runtime_history_t *history, uint32_t now_ms);
bool vp_runtime_history_take_summary(vp_runtime_history_t *history, uint32_t now_ms,
                                     vp_runtime_history_summary_t *out_summary);

#endif
