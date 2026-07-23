#ifndef EPD_UI_RENDERER_H
#define EPD_UI_RENDERER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VP_UI_WIDTH 250
#define VP_UI_HEIGHT 122
#define VP_UI_NATIVE_STRIDE 16
#define VP_UI_PLANE_SIZE 4000
#define VP_UI_IMAGE_SIZE (VP_UI_PLANE_SIZE * 2)
#define VP_UI_PLANE_BLACK 0
#define VP_UI_PLANE_RED 1

typedef enum {
    VP_UI_STATE_STANDBY = 0,
    VP_UI_STATE_PREPARE,
    VP_UI_STATE_RUNNING,
    VP_UI_STATE_FAULT,
    VP_UI_STATE_SHUTDOWN,
} vp_ui_state_t;

typedef struct {
    bool bms_valid;
    bool gear_valid;
    uint8_t soc_percent;
    uint8_t gear;
    int32_t voltage_mv;
    int32_t current_ma;
    bool charge_mos_active;
    vp_ui_state_t state;
    uint16_t fault_code;
} vp_ui_snapshot_t;

void vp_epd_ui_render(const vp_ui_snapshot_t *snapshot, uint8_t *image, size_t image_size);
bool vp_epd_ui_needs_refresh(const vp_ui_snapshot_t *old_value,
                             const vp_ui_snapshot_t *new_value);
bool vp_epd_ui_fault_changed(const vp_ui_snapshot_t *old_value,
                             const vp_ui_snapshot_t *new_value);

#endif
