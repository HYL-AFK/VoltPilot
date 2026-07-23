#ifndef APP_STATE_SERVICE_H
#define APP_STATE_SERVICE_H

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    VP_APP_STATE_BOOT = 0,
    VP_APP_STATE_STANDBY,
    VP_APP_STATE_PREPARE,
    VP_APP_STATE_RUNNING,
    VP_APP_STATE_FAULT,
    VP_APP_STATE_SHUTDOWN,
} vp_app_state_t;

typedef enum {
    VP_APP_EVENT_BUTTON_SINGLE = 0,
    VP_APP_EVENT_BUTTON_DOUBLE,
    VP_APP_EVENT_BUTTON_LONG,
    VP_APP_EVENT_ADC_UPDATE,
    VP_APP_EVENT_BMS_RX,
    VP_APP_EVENT_BMS_TIMEOUT,
    VP_APP_EVENT_STC_RX,
    VP_APP_EVENT_STC_TIMEOUT,
    VP_APP_EVENT_AI_RS485_OFFLINE,
    VP_APP_EVENT_AI_RS485_RECOVERED,
    VP_APP_EVENT_FAULT,
} vp_app_event_id_t;

esp_err_t app_state_service_init(void);
esp_err_t app_state_post_event(vp_app_event_id_t id, int32_t value);
vp_app_state_t app_state_service_get_state(void);
const char *app_state_name(vp_app_state_t state);

#endif
