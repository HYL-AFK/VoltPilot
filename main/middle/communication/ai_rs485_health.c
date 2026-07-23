#include "ai_rs485_health.h"

#include <stddef.h>

void vp_ai_rs485_health_init(vp_ai_rs485_health_t *health, uint8_t failure_limit)
{
    if (health == NULL) {
        return;
    }
    health->consecutive_failures = 0;
    health->failure_limit = failure_limit == 0 ? 1 : failure_limit;
    health->ready = false;
    health->offline_reported = false;
}

vp_ai_rs485_health_event_t vp_ai_rs485_health_note_success(vp_ai_rs485_health_t *health)
{
    if (health == NULL) {
        return VP_AI_RS485_HEALTH_NO_CHANGE;
    }
    bool was_ready = health->ready;
    health->consecutive_failures = 0;
    health->ready = true;
    health->offline_reported = false;
    return was_ready ? VP_AI_RS485_HEALTH_NO_CHANGE : VP_AI_RS485_HEALTH_RECOVERED;
}

vp_ai_rs485_health_event_t vp_ai_rs485_health_note_failure(vp_ai_rs485_health_t *health)
{
    if (health == NULL) {
        return VP_AI_RS485_HEALTH_NO_CHANGE;
    }
    if (health->consecutive_failures < health->failure_limit) {
        health->consecutive_failures++;
    }
    if (health->consecutive_failures < health->failure_limit || health->offline_reported) {
        return VP_AI_RS485_HEALTH_NO_CHANGE;
    }
    health->ready = false;
    health->offline_reported = true;
    return VP_AI_RS485_HEALTH_OFFLINE;
}

bool vp_ai_rs485_health_is_ready(const vp_ai_rs485_health_t *health)
{
    return health != NULL && health->ready;
}
