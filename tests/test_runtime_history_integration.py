from pathlib import Path


ROOT = Path(__file__).parents[1]
DIAG_SOURCE = (ROOT / "main" / "middle" / "diagnostics" / "diag_service.c").read_text(encoding="utf-8")
DIAG_HEADER = (ROOT / "main" / "middle" / "diagnostics" / "diag_service.h").read_text(encoding="utf-8")
APP_SOURCE = (ROOT / "main" / "app" / "app_start.c").read_text(encoding="utf-8")


def test_runtime_history_is_sampled_each_second_and_events_are_persisted() -> None:
    """运行历史须独立于通信服务，每秒采样，并在关键事件时立即保存。"""
    assert '#include "runtime_history.h"' in DIAG_SOURCE
    assert "esp_err_t diag_service_tick(void);" in DIAG_HEADER
    assert "diag_service_tick()" in APP_SOURCE
    assert "pdMS_TO_TICKS(1000)" in APP_SOURCE
    assert "capture_runtime_record(true" in DIAG_SOURCE


def test_runtime_history_has_five_minute_summary_storage() -> None:
    """五分钟统计必须写入独立 NVS 键，不能挤占运行快照语义。"""
    assert 'RUNTIME_SUMMARY_KEY "summary"' in DIAG_SOURCE
    assert "vp_runtime_history_take_summary" in DIAG_SOURCE


if __name__ == "__main__":
    test_runtime_history_is_sampled_each_second_and_events_are_persisted()
    test_runtime_history_has_five_minute_summary_storage()
    print("runtime history integration tests passed")
