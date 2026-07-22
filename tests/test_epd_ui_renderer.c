#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "epd_ui_font.h"
#include "epd_ui_renderer.h"

/* 使用终端错误信息代替 Windows CRT assert 弹窗，避免自动测试被阻塞。 */
#define assert(expression) do {                                                    \
    if (!(expression)) {                                                          \
        fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #expression, __FILE__,   \
                __LINE__);                                                        \
        exit(EXIT_FAILURE);                                                       \
    }                                                                             \
} while (0)

static int pixel_is_set(const uint8_t *image, int plane, int x, int y)
{
    int native_x = y;
    int native_y = 249 - x;
    size_t offset = (size_t)plane * VP_UI_PLANE_SIZE +
                    (size_t)native_y * VP_UI_NATIVE_STRIDE + (size_t)(native_x / 8);
    return (image[offset] & (uint8_t)(0x80U >> (native_x % 8))) == 0;
}

static vp_ui_snapshot_t valid_snapshot(void)
{
    return (vp_ui_snapshot_t){
        .bms_valid = true,
        .gear_valid = true,
        .soc_percent = 100,
        .gear = 2,
        .voltage_mv = 55100,
        .current_ma = 0,
        .charge_mos_active = false,
        .state = VP_UI_STATE_RUNNING,
    };
}

static int plane_pixel_count(const uint8_t *image, int plane)
{
    int count = 0;
    for (int y = 0; y < VP_UI_HEIGHT; ++y) {
        for (int x = 0; x < VP_UI_WIDTH; ++x) {
            count += pixel_is_set(image, plane, x, y);
        }
    }
    return count;
}

static int column_has_pixel(const uint8_t *image, int plane, int x, int y0, int y1)
{
    for (int y = y0; y <= y1; ++y) {
        if (pixel_is_set(image, plane, x, y)) {
            return 1;
        }
    }
    return 0;
}

static int glyph_pixel_count(const vp_ui_glyph_t *glyph)
{
    int count = 0;
    for (int y = 0; y < glyph->height; ++y) {
        for (int x = 0; x < glyph->width; ++x) {
            size_t offset = (size_t)y * glyph->stride + (size_t)(x / 8);
            count += (glyph->bitmap[offset] & (uint8_t)(0x80U >> (x % 8))) != 0;
        }
    }
    return count;
}

static void test_render_stays_inside_exact_framebuffer(void)
{
    uint8_t guarded[VP_UI_IMAGE_SIZE + 2];
    memset(guarded, 0xA5, sizeof(guarded));
    vp_ui_snapshot_t snapshot = valid_snapshot();
    vp_epd_ui_render(&snapshot, guarded + 1, VP_UI_IMAGE_SIZE);
    assert(guarded[0] == 0xA5);
    assert(guarded[VP_UI_IMAGE_SIZE + 1] == 0xA5);
}

static void test_new_font_subset_is_available(void)
{
    vp_ui_glyph_t glyph;
    for (const char *p = "0123456789"; *p != '\0'; ++p) {
        assert(vp_ui_font_get_glyph(VP_UI_FONT_SOC, *p, &glyph));
        assert(glyph.height >= 72);
    }
    assert(vp_ui_font_get_glyph(VP_UI_FONT_PERCENT, '%', &glyph));
    for (const char *p = "23468V"; *p != '\0'; ++p) {
        assert(vp_ui_font_get_glyph(VP_UI_FONT_GEAR, *p, &glyph));
        assert(glyph.height >= 12);
    }
    for (const char *p = "RPSFO"; *p != '\0'; ++p) {
        assert(vp_ui_font_get_glyph(VP_UI_FONT_STATE, *p, &glyph));
        assert(glyph.height >= 19);
    }
    assert(vp_ui_font_get_glyph(VP_UI_FONT_GEAR, 'V', &glyph));
    assert(glyph_pixel_count(&glyph) >= 45 && glyph_pixel_count(&glyph) <= 65);
    assert(vp_ui_font_get_glyph(VP_UI_FONT_METRIC, '8', &glyph));
    assert(glyph_pixel_count(&glyph) >= 135 && glyph_pixel_count(&glyph) <= 180);
    assert(vp_ui_font_get_glyph(VP_UI_FONT_STATE, 'F', &glyph));
    assert(glyph_pixel_count(&glyph) >= 100 && glyph_pixel_count(&glyph) <= 140);
}

static void test_layout_matches_confirmed_sketch(void)
{
    uint8_t image[VP_UI_IMAGE_SIZE];
    vp_ui_snapshot_t snapshot = valid_snapshot();
    vp_epd_ui_render(&snapshot, image, sizeof(image));

    /* 左右主区、底栏和状态小格的固定分隔线。 */
    assert(pixel_is_set(image, VP_UI_PLANE_BLACK, 170, 40));
    assert(pixel_is_set(image, VP_UI_PLANE_BLACK, 100, 95));
    assert(pixel_is_set(image, VP_UI_PLANE_BLACK, 225, 108));
    for (int x = 226; x < 247; ++x) {
        assert(!pixel_is_set(image, VP_UI_PLANE_BLACK, x, 117));
    }

    /* 三档区只占右侧主区约 60%，下半区保持空白。 */
    assert(pixel_is_set(image, VP_UI_PLANE_BLACK, 200, 23));
    assert(pixel_is_set(image, VP_UI_PLANE_BLACK, 200, 44));
    assert(pixel_is_set(image, VP_UI_PLANE_BLACK, 200, 65));
    int lower_row_pixels = 0;
    for (int x = 171; x < 247; ++x) {
        lower_row_pixels += pixel_is_set(image, VP_UI_PLANE_BLACK, x, 75);
    }
    assert(lower_row_pixels < 40);

    /* 十格电量条必须有九条内部竖线。 */
    static const int dividers[] = {19, 36, 53, 69, 86, 103, 119, 136, 153};
    for (size_t i = 0; i < sizeof(dividers) / sizeof(dividers[0]); ++i) {
        assert(pixel_is_set(image, VP_UI_PLANE_BLACK, dividers[i], 108));
    }
}

static void test_outer_frame_is_removed_and_status_keeps_safe_margin(void)
{
    uint8_t image[VP_UI_IMAGE_SIZE];
    vp_ui_snapshot_t snapshot = valid_snapshot();
    vp_epd_ui_render(&snapshot, image, sizeof(image));

    assert(!pixel_is_set(image, VP_UI_PLANE_BLACK, 2, 2));
    assert(!pixel_is_set(image, VP_UI_PLANE_BLACK, 247, 119));
    assert(!pixel_is_set(image, VP_UI_PLANE_BLACK, 2, 60));
    assert(!pixel_is_set(image, VP_UI_PLANE_BLACK, 247, 80));

    for (int y = 96; y < 119; ++y) {
        for (int x = 244; x < 250; ++x) {
            assert(!pixel_is_set(image, VP_UI_PLANE_BLACK, x, y));
        }
    }
}

static void test_active_gear_marker_is_nine_pixels_square(void)
{
    uint8_t image[VP_UI_IMAGE_SIZE];
    vp_ui_snapshot_t snapshot = valid_snapshot();
    vp_epd_ui_render(&snapshot, image, sizeof(image));

    for (int y = 30; y <= 38; ++y) {
        for (int x = 231; x <= 239; ++x) {
            assert(pixel_is_set(image, VP_UI_PLANE_BLACK, x, y));
        }
    }
}

static void test_soc_is_large_and_normal_screen_has_no_red(void)
{
    uint8_t image[VP_UI_IMAGE_SIZE];
    vp_ui_snapshot_t snapshot = valid_snapshot();
    vp_epd_ui_render(&snapshot, image, sizeof(image));

    int soc_pixels = 0;
    for (int y = 8; y < 91; ++y) {
        for (int x = 8; x < 166; ++x) {
            soc_pixels += pixel_is_set(image, VP_UI_PLANE_BLACK, x, y);
        }
    }
    assert(soc_pixels > 1300);
    /* 100 按“1”和“00”两个字块紧凑绘制，不能保留原来的 17px 空洞。 */
    assert(column_has_pixel(image, VP_UI_PLANE_BLACK, 58, 10, 80));
    assert(plane_pixel_count(image, VP_UI_PLANE_RED) == 0);

    int top = VP_UI_HEIGHT;
    int bottom = -1;
    for (int y = 0; y < 95; ++y) {
        for (int x = 3; x < 170; ++x) {
            if (pixel_is_set(image, VP_UI_PLANE_BLACK, x, y)) {
                if (y < top) top = y;
                if (y > bottom) bottom = y;
            }
        }
    }
    assert(top <= 10);
    assert(bottom <= 84);
}

static void test_low_soc_and_fault_are_the_only_red_states(void)
{
    uint8_t image[VP_UI_IMAGE_SIZE];
    vp_ui_snapshot_t snapshot = valid_snapshot();

    snapshot.soc_percent = 20;
    vp_epd_ui_render(&snapshot, image, sizeof(image));
    assert(plane_pixel_count(image, VP_UI_PLANE_RED) == 0);

    snapshot.soc_percent = 10;
    vp_epd_ui_render(&snapshot, image, sizeof(image));
    assert(pixel_is_set(image, VP_UI_PLANE_RED, 10, 108));
    assert(plane_pixel_count(image, VP_UI_PLANE_RED) > 100);

    snapshot.soc_percent = 78;
    snapshot.state = VP_UI_STATE_FAULT;
    vp_epd_ui_render(&snapshot, image, sizeof(image));
    for (int y = 97; y <= 119; ++y) {
        for (int x = 172; x <= 223; ++x) {
            assert(pixel_is_set(image, VP_UI_PLANE_RED, x, y));
        }
    }
    assert(!pixel_is_set(image, VP_UI_PLANE_RED, 171, 108));
    assert(!pixel_is_set(image, VP_UI_PLANE_RED, 224, 108));
    assert(!pixel_is_set(image, VP_UI_PLANE_RED, 180, 96));
    assert(!pixel_is_set(image, VP_UI_PLANE_RED, 180, 120));
    assert(plane_pixel_count(image, VP_UI_PLANE_RED) == 52 * 23);
}

static void test_voltage_appears_below_gear_rows(void)
{
    uint8_t image[VP_UI_IMAGE_SIZE];
    vp_ui_snapshot_t snapshot = valid_snapshot();
    vp_epd_ui_render(&snapshot, image, sizeof(image));

    int pixels = 0;
    for (int y = 66; y < 95; ++y) {
        for (int x = 171; x < 247; ++x) {
            pixels += pixel_is_set(image, VP_UI_PLANE_BLACK, x, y);
        }
    }
    assert(pixels > 120);

    snapshot.bms_valid = false;
    vp_epd_ui_render(&snapshot, image, sizeof(image));
    pixels = 0;
    for (int y = 66; y < 95; ++y) {
        for (int x = 171; x < 247; ++x) {
            pixels += pixel_is_set(image, VP_UI_PLANE_BLACK, x, y);
        }
    }
    assert(pixels > 40);
}

static void test_low_soc_charge_mos_uses_charging_page_and_fault_wins(void)
{
    uint8_t image[VP_UI_IMAGE_SIZE];
    vp_ui_snapshot_t snapshot = valid_snapshot();
    snapshot.soc_percent = 10;
    vp_epd_ui_render(&snapshot, image, sizeof(image));
    assert(pixel_is_set(image, VP_UI_PLANE_BLACK, 170, 40));

    snapshot.charge_mos_active = true;
    vp_epd_ui_render(&snapshot, image, sizeof(image));

    /* 充电页不保留主页面分隔线，只显示放大的电池和闪电图形。 */
    assert(!pixel_is_set(image, VP_UI_PLANE_BLACK, 170, 40));
    assert(pixel_is_set(image, VP_UI_PLANE_BLACK, 30, 27));
    assert(pixel_is_set(image, VP_UI_PLANE_BLACK, 30, 94));
    assert(pixel_is_set(image, VP_UI_PLANE_RED, 34, 38));
    int charging_text_pixels = 0;
    for (int y = 96; y < 122; ++y) {
        for (int x = 0; x < VP_UI_WIDTH; ++x) {
            if (pixel_is_set(image, VP_UI_PLANE_BLACK, x, y)) {
                charging_text_pixels++;
            }
        }
    }
    assert(charging_text_pixels == 0);

    snapshot.soc_percent = 20;
    vp_epd_ui_render(&snapshot, image, sizeof(image));
    assert(pixel_is_set(image, VP_UI_PLANE_BLACK, 170, 40));

    snapshot.soc_percent = 10;
    snapshot.state = VP_UI_STATE_FAULT;
    vp_epd_ui_render(&snapshot, image, sizeof(image));
    assert(pixel_is_set(image, VP_UI_PLANE_BLACK, 170, 40));
    assert(pixel_is_set(image, VP_UI_PLANE_RED, 180, 108));
}

static void test_refresh_policy_thresholds(void)
{
    vp_ui_snapshot_t old_value = valid_snapshot();
    vp_ui_snapshot_t new_value = old_value;
    assert(!vp_epd_ui_needs_refresh(&old_value, &new_value));
    new_value.soc_percent = 10;
    assert(vp_epd_ui_needs_refresh(&old_value, &new_value));
    new_value = old_value;
    new_value.state = VP_UI_STATE_FAULT;
    assert(vp_epd_ui_fault_changed(&old_value, &new_value));

    new_value = old_value;
    new_value.voltage_mv += 499;
    assert(!vp_epd_ui_needs_refresh(&old_value, &new_value));
    new_value.voltage_mv += 1;
    assert(vp_epd_ui_needs_refresh(&old_value, &new_value));

    new_value = old_value;
    new_value.charge_mos_active = true;
    assert(vp_epd_ui_needs_refresh(&old_value, &new_value));
}

int main(void)
{
    test_new_font_subset_is_available();
    test_render_stays_inside_exact_framebuffer();
    test_layout_matches_confirmed_sketch();
    test_outer_frame_is_removed_and_status_keeps_safe_margin();
    test_active_gear_marker_is_nine_pixels_square();
    test_soc_is_large_and_normal_screen_has_no_red();
    test_low_soc_and_fault_are_the_only_red_states();
    test_voltage_appears_below_gear_rows();
    test_low_soc_charge_mos_uses_charging_page_and_fault_wins();
    test_refresh_policy_thresholds();
    puts("epd_ui_renderer tests passed");
    return 0;
}
