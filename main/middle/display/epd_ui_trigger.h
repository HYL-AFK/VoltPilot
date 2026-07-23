#ifndef EPD_UI_TRIGGER_H
#define EPD_UI_TRIGGER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool active;
    bool transition_pending;
    uint32_t transition_started_ms;
} vp_ui_charging_trigger_t;

typedef struct {
    uint32_t enter_stable_ms;
    uint32_t exit_stable_ms;
    uint8_t enter_soc_percent;
    uint8_t exit_soc_percent;
} vp_ui_charging_trigger_config_t;

void vp_ui_charging_trigger_reset(vp_ui_charging_trigger_t *trigger);
bool vp_ui_charging_trigger_update(vp_ui_charging_trigger_t *trigger, bool bms_valid,
                                   uint8_t soc_percent, bool charging_signal,
                                   uint32_t now_ms,
                                   const vp_ui_charging_trigger_config_t *config);

#endif
