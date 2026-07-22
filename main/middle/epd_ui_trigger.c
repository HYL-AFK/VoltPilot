#include "epd_ui_trigger.h"

#include <string.h>

void vp_ui_charging_trigger_reset(vp_ui_charging_trigger_t *trigger)
{
    if (trigger != NULL) {
        memset(trigger, 0, sizeof(*trigger));
    }
}

bool vp_ui_charging_trigger_update(vp_ui_charging_trigger_t *trigger, bool bms_valid,
                                   uint8_t soc_percent, bool charging_signal,
                                   uint32_t now_ms,
                                   const vp_ui_charging_trigger_config_t *config)
{
    if (trigger == NULL || config == NULL) {
        return false;
    }

    if (trigger->active) {
        if (!bms_valid || soc_percent >= config->exit_soc_percent) {
            trigger->active = false;
            trigger->transition_pending = false;
        } else if (charging_signal) {
            trigger->transition_pending = false;
        } else if (!trigger->transition_pending) {
            trigger->transition_pending = true;
            trigger->transition_started_ms = now_ms;
        } else if ((uint32_t)(now_ms - trigger->transition_started_ms) >=
                   config->exit_stable_ms) {
            trigger->active = false;
            trigger->transition_pending = false;
        }
        return trigger->active;
    }

    bool enter_requested = bms_valid && soc_percent < config->enter_soc_percent &&
                           charging_signal;
    if (!enter_requested) {
        trigger->transition_pending = false;
    } else if (!trigger->transition_pending) {
        trigger->transition_pending = true;
        trigger->transition_started_ms = now_ms;
    } else if ((uint32_t)(now_ms - trigger->transition_started_ms) >=
               config->enter_stable_ms) {
        trigger->active = true;
        trigger->transition_pending = false;
    }
    return trigger->active;
}
