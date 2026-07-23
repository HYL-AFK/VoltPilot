#include "runtime_history.h"

#include <limits.h>
#include <string.h>

void vp_runtime_history_init(vp_runtime_history_t *history)
{
    if (history != NULL) {
        memset(history, 0, sizeof(*history));
    }
}

bool vp_runtime_history_push_ram(vp_runtime_history_t *history,
                                 const vp_runtime_history_record_t *record)
{
    if (history == NULL || record == NULL) {
        return false;
    }

    history->ram_records[history->ram_head] = *record;
    history->ram_head = (uint16_t)((history->ram_head + 1U) % VP_RUNTIME_HISTORY_RAM_CAPACITY);
    if (history->ram_count < VP_RUNTIME_HISTORY_RAM_CAPACITY) {
        history->ram_count++;
    }
    return true;
}

size_t vp_runtime_history_ram_count(const vp_runtime_history_t *history)
{
    return history != NULL ? history->ram_count : 0U;
}

bool vp_runtime_history_get_ram(const vp_runtime_history_t *history, size_t index,
                                vp_runtime_history_record_t *out_record)
{
    if (history == NULL || out_record == NULL || index >= history->ram_count) {
        return false;
    }

    uint16_t oldest = history->ram_count == VP_RUNTIME_HISTORY_RAM_CAPACITY ?
                          history->ram_head : 0U;
    uint16_t physical = (uint16_t)((oldest + index) % VP_RUNTIME_HISTORY_RAM_CAPACITY);
    *out_record = history->ram_records[physical];
    return true;
}

bool vp_runtime_history_get_latest(const vp_runtime_history_t *history,
                                   vp_runtime_history_record_t *out_record)
{
    if (history == NULL || history->ram_count == 0U || out_record == NULL) {
        return false;
    }
    uint16_t latest = history->ram_head == 0U ?
                          VP_RUNTIME_HISTORY_RAM_CAPACITY - 1U : history->ram_head - 1U;
    *out_record = history->ram_records[latest];
    return true;
}

bool vp_runtime_history_should_persist(const vp_runtime_history_t *history,
                                       const vp_runtime_history_record_t *record,
                                       bool force_event)
{
    if (history == NULL || record == NULL) {
        return false;
    }
    return force_event || !history->persisted_once ||
           (record->uptime_ms - history->last_persist_ms) >= VP_RUNTIME_HISTORY_FLASH_INTERVAL_MS;
}

void vp_runtime_history_mark_persisted(vp_runtime_history_t *history,
                                       const vp_runtime_history_record_t *record)
{
    if (history != NULL && record != NULL) {
        history->persisted_once = true;
        history->last_persist_ms = record->uptime_ms;
    }
}

void vp_runtime_history_observe(vp_runtime_history_t *history,
                                const vp_runtime_history_record_t *record)
{
    if (history == NULL || record == NULL) {
        return;
    }
    if (!history->summary_active) {
        history->summary_active = true;
        history->summary.window_start_ms = record->uptime_ms;
        history->summary.min_pack_mv = INT32_MAX;
        history->summary.max_pack_mv = INT32_MIN;
        history->summary.min_soc_percent = UINT8_MAX;
        history->summary.max_temperature_c = INT8_MIN;
    }

    if (record->pack_mv < history->summary.min_pack_mv) {
        history->summary.min_pack_mv = record->pack_mv;
    }
    if (record->pack_mv > history->summary.max_pack_mv) {
        history->summary.max_pack_mv = record->pack_mv;
    }
    if (record->soc_percent < history->summary.min_soc_percent) {
        history->summary.min_soc_percent = record->soc_percent;
    }
    if (record->max_temperature_c > history->summary.max_temperature_c) {
        history->summary.max_temperature_c = record->max_temperature_c;
    }
    if (record->record_type == VP_RUNTIME_HISTORY_RECORD_EVENT) {
        history->summary.event_count++;
    }
    history->summary.sample_count++;
    history->summary.window_end_ms = record->uptime_ms;
}

bool vp_runtime_history_summary_due(const vp_runtime_history_t *history, uint32_t now_ms)
{
    return history != NULL && history->summary_active &&
           (now_ms - history->summary.window_start_ms) >= VP_RUNTIME_HISTORY_SUMMARY_INTERVAL_MS;
}

bool vp_runtime_history_take_summary(vp_runtime_history_t *history, uint32_t now_ms,
                                     vp_runtime_history_summary_t *out_summary)
{
    if (!vp_runtime_history_summary_due(history, now_ms) || out_summary == NULL) {
        return false;
    }
    history->summary.window_end_ms = now_ms;
    *out_summary = history->summary;
    history->summary_active = false;
    memset(&history->summary, 0, sizeof(history->summary));
    return true;
}
