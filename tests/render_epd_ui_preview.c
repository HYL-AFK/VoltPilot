#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "epd_ui_renderer.h"

static int pixel_is_set(const uint8_t *image, int plane, int x, int y)
{
    int native_x = y;
    int native_y = 249 - x;
    size_t offset = (size_t)plane * VP_UI_PLANE_SIZE +
                    (size_t)native_y * VP_UI_NATIVE_STRIDE + (size_t)(native_x / 8);
    return (image[offset] & (uint8_t)(0x80U >> (native_x % 8))) == 0;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        return 2;
    }

    uint8_t image[VP_UI_IMAGE_SIZE];
    vp_ui_snapshot_t snapshot = {
        .bms_valid = true,
        .gear_valid = true,
        .soc_percent = 100,
        .gear = 2,
        .voltage_mv = 55100,
        .current_ma = 0,
        .charge_mos_active = false,
        .state = VP_UI_STATE_RUNNING,
    };
    if (argc == 3 && strcmp(argv[2], "fault") == 0) {
        snapshot.soc_percent = 100;
        snapshot.state = VP_UI_STATE_FAULT;
        snapshot.fault_code = 1;
    } else if (argc == 3 && strcmp(argv[2], "charging") == 0) {
        snapshot.soc_percent = 10;
        snapshot.charge_mos_active = true;
    }
    vp_epd_ui_render(&snapshot, image, sizeof(image));

    FILE *output = fopen(argv[1], "wb");
    if (output == NULL) {
        return 3;
    }
    fprintf(output, "P6\n%d %d\n255\n", VP_UI_WIDTH, VP_UI_HEIGHT);
    for (int y = 0; y < VP_UI_HEIGHT; ++y) {
        for (int x = 0; x < VP_UI_WIDTH; ++x) {
            const uint8_t black[3] = {15, 15, 15};
            const uint8_t red[3] = {190, 12, 30};
            const uint8_t white[3] = {255, 255, 255};
            const uint8_t *color = pixel_is_set(image, VP_UI_PLANE_RED, x, y) ? red :
                                   pixel_is_set(image, VP_UI_PLANE_BLACK, x, y) ? black : white;
            fwrite(color, 1, 3, output);
        }
    }
    fclose(output);
    return 0;
}
