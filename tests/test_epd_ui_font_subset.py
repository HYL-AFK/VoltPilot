from pathlib import Path


ROOT = Path(__file__).parents[1]
FONT_HEADER = ROOT / "main" / "middle" / "display" / "epd_ui_font.h"
FONT_SOURCE = ROOT / "main" / "middle" / "display" / "epd_ui_font.c"


def test_generated_font_keeps_only_renderer_subsets() -> None:
    """The customer UI must not retain glyph tables that no screen can render."""
    header = FONT_HEADER.read_text(encoding="utf-8")
    source = FONT_SOURCE.read_text(encoding="utf-8")

    for font in ("METRIC", "SOC", "PERCENT", "GEAR", "STATE"):
        assert f"VP_UI_FONT_{font}" in header

    for font in ("SMALL", "BRAND", "CHARGING"):
        assert f"VP_UI_FONT_{font}" not in header

    for table in ("s_small_", "s_brand_", "s_charging_"):
        assert table not in source


if __name__ == "__main__":
    test_generated_font_keeps_only_renderer_subsets()
    print("epd_ui_font_subset tests passed")
