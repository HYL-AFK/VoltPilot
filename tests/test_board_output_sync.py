from pathlib import Path


ROOT = Path(__file__).parents[1]
SOURCE = (ROOT / "main" / "middle" / "hardware" / "board_service.c").read_text(encoding="utf-8")


def _function_body(name: str) -> str:
    start = SOURCE.index(f"esp_err_t {name}(")
    brace = SOURCE.index("{", start)
    depth = 0
    for index in range(brace, len(SOURCE)):
        if SOURCE[index] == "{":
            depth += 1
        elif SOURCE[index] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[brace:index + 1]
    raise AssertionError(f"function {name} has no closing brace")


def test_en_outputs_share_a_mutex_and_all_off_is_atomic() -> None:
    """EN 切换与安全全关断必须使用同一把 FreeRTOS mutex。"""
    assert "static SemaphoreHandle_t s_output_mutex;" in SOURCE
    assert "xSemaphoreCreateMutex()" in SOURCE

    shared_setter = _function_body("board_set_output")
    assert "output_lock()" in shared_setter
    assert "output_unlock()" in shared_setter

    for name in (
        "board_output_set_24v",
        "board_output_set_36v",
        "board_output_set_48v",
    ):
        body = _function_body(name)
        assert "board_set_output(" in body

    all_off = _function_body("board_output_all_off")
    assert "output_lock()" in all_off
    assert "output_unlock()" in all_off
    assert "board_output_set_24v(" not in all_off
    assert "board_output_set_36v(" not in all_off
    assert "board_output_set_48v(" not in all_off


def test_en_state_updates_only_after_gpio_write_succeeds() -> None:
    """GPIO 写失败时，内存中的 EN 状态不能被提前篡改。"""
    body = SOURCE[SOURCE.index("static esp_err_t board_set_output"):SOURCE.index("esp_err_t board_output_set_24v")]
    gpio_write = body.index("gpio_set_level")
    state_update = body.index("*state = enable")
    assert gpio_write < state_update


if __name__ == "__main__":
    test_en_outputs_share_a_mutex_and_all_off_is_atomic()
    test_en_state_updates_only_after_gpio_write_succeeds()
    print("board output synchronization tests passed")
