#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "runtime_history.h"

#define assert(expression) do {                                                    \
    if (!(expression)) {                                                          \
        fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #expression, __FILE__,   \
                __LINE__);                                                        \
        exit(EXIT_FAILURE);                                                        \
    }                                                                             \
} while (0)

static vp_runtime_history_record_t make_record(uint32_t uptime_ms, uint8_t soc)
{
    return (vp_runtime_history_record_t){
        .uptime_ms = uptime_ms,
        .soc_percent = soc,
    };
}

static void test_ram_history_keeps_latest_records(void)
{
    vp_runtime_history_t history;
    vp_runtime_history_init(&history);

    for (uint32_t second = 0; second < VP_RUNTIME_HISTORY_RAM_CAPACITY + 2U; second++) {
        vp_runtime_history_record_t record = make_record(second * 1000U, (uint8_t)second);
        assert(vp_runtime_history_push_ram(&history, &record));
    }

    assert(vp_runtime_history_ram_count(&history) == VP_RUNTIME_HISTORY_RAM_CAPACITY);
    vp_runtime_history_record_t oldest = {0};
    vp_runtime_history_record_t latest = {0};
    assert(vp_runtime_history_get_ram(&history, 0, &oldest));
    assert(vp_runtime_history_get_latest(&history, &latest));
    assert(oldest.uptime_ms == 2000U);
    assert(latest.uptime_ms == (VP_RUNTIME_HISTORY_RAM_CAPACITY + 1U) * 1000U);
}

static void test_snapshot_and_event_persistence_policy(void)
{
    vp_runtime_history_t history;
    vp_runtime_history_init(&history);
    vp_runtime_history_record_t record = make_record(0, 50);

    assert(vp_runtime_history_should_persist(&history, &record, false));
    vp_runtime_history_mark_persisted(&history, &record);

    record.uptime_ms = VP_RUNTIME_HISTORY_FLASH_INTERVAL_MS - 1U;
    assert(!vp_runtime_history_should_persist(&history, &record, false));
    assert(vp_runtime_history_should_persist(&history, &record, true));

    record.uptime_ms = VP_RUNTIME_HISTORY_FLASH_INTERVAL_MS;
    assert(vp_runtime_history_should_persist(&history, &record, false));
}

static void test_summary_tracks_five_minute_extremes(void)
{
    vp_runtime_history_t history;
    vp_runtime_history_init(&history);

    vp_runtime_history_record_t first = make_record(0, 76);
    first.pack_mv = 42000;
    first.max_temperature_c = 31;
    vp_runtime_history_observe(&history, &first);

    vp_runtime_history_record_t second = make_record(120000U, 51);
    second.pack_mv = 39800;
    second.max_temperature_c = 43;
    vp_runtime_history_observe(&history, &second);
    assert(!vp_runtime_history_summary_due(&history, second.uptime_ms));

    assert(vp_runtime_history_summary_due(&history, VP_RUNTIME_HISTORY_SUMMARY_INTERVAL_MS));
    vp_runtime_history_summary_t summary = {0};
    assert(vp_runtime_history_take_summary(&history, VP_RUNTIME_HISTORY_SUMMARY_INTERVAL_MS,
                                           &summary));
    assert(summary.min_soc_percent == 51);
    assert(summary.max_temperature_c == 43);
    assert(summary.min_pack_mv == 39800);
    assert(summary.max_pack_mv == 42000);
}

int main(void)
{
    test_ram_history_keeps_latest_records();
    test_snapshot_and_event_persistence_policy();
    test_summary_tracks_five_minute_extremes();
    puts("runtime history tests passed");
    return 0;
}
