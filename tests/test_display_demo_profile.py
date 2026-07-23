from pathlib import Path


ROOT = Path(__file__).parents[1]
BOARD = (ROOT / "main" / "common" / "vp_board.h").read_text(encoding="utf-8")
DISPLAY = (ROOT / "main" / "middle" / "display" / "display_service.c").read_text(encoding="utf-8")


def test_leadership_demo_profile_shows_9_percent_24v_fault() -> None:
    """领导演示期间，屏幕必须脱离真实通信并固定显示目标画面。"""
    assert "#define VP_EPD_UI_DEMO_DATA 1" in BOARD
    assert ".soc_percent = 9," in DISPLAY
    assert ".gear = 1," in DISPLAY
    assert ".charge_mos_active = false," in DISPLAY
    assert ".state = VP_UI_STATE_FAULT," in DISPLAY
    assert ".fault_code = 1," in DISPLAY


def test_charging_configuration_is_excluded_from_demo_build() -> None:
    """演示模式不编译真实充电判断专用配置，避免 -Werror 未使用变量错误。"""
    marker = "#if !VP_EPD_UI_DEMO_DATA\nstatic const vp_ui_charging_trigger_config_t s_charging_trigger_config"
    assert marker in DISPLAY


if __name__ == "__main__":
    test_leadership_demo_profile_shows_9_percent_24v_fault()
    test_charging_configuration_is_excluded_from_demo_build()
    print("display demo profile tests passed")
