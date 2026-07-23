#ifndef AI_RS485_HEALTH_H
#define AI_RS485_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    VP_AI_RS485_HEALTH_NO_CHANGE = 0,
    VP_AI_RS485_HEALTH_OFFLINE,
    VP_AI_RS485_HEALTH_RECOVERED,
} vp_ai_rs485_health_event_t;

typedef struct {
    uint8_t consecutive_failures;
    uint8_t failure_limit;
    bool ready;
    bool offline_reported;
} vp_ai_rs485_health_t;

void vp_ai_rs485_health_init(vp_ai_rs485_health_t *health, uint8_t failure_limit);
vp_ai_rs485_health_event_t vp_ai_rs485_health_note_success(vp_ai_rs485_health_t *health);
vp_ai_rs485_health_event_t vp_ai_rs485_health_note_failure(vp_ai_rs485_health_t *health);
bool vp_ai_rs485_health_is_ready(const vp_ai_rs485_health_t *health);

#endif
