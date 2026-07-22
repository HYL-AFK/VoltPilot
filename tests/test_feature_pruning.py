from pathlib import Path


ROOT = Path(__file__).parents[1]
MAIN = ROOT / "main"


def test_project_keeps_no_unused_bring_up_features() -> None:
    """Only shipped hardware services may remain in the production source tree."""
    defaults = ROOT / "sdkconfig.defaults"
    assert defaults.exists()
    sources = [
        defaults,
        MAIN / "CMakeLists.txt",
        *MAIN.rglob("*.c"),
        *MAIN.rglob("*.h"),
    ]
    content = "\n".join(path.read_text(encoding="utf-8") for path in sources)

    assert "CONFIG_ESP_WIFI_ENABLED=n" in content
    assert "esp_driver_i2c" not in content
    assert "VP_I2C_" not in content


if __name__ == "__main__":
    test_project_keeps_no_unused_bring_up_features()
    print("feature pruning tests passed")
