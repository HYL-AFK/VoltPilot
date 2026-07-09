#ifndef FAULT_SERVICE_H
#define FAULT_SERVICE_H

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    VP_FAULT_NONE = 0,
    VP_FAULT_BMS_TIMEOUT,
    VP_FAULT_STC_TIMEOUT,
    VP_FAULT_ADC_ERROR,
    VP_FAULT_OUTPUT_ERROR,
    VP_FAULT_USER_REQUEST,
} vp_fault_code_t;

esp_err_t fault_service_init(void);
void fault_service_raise(vp_fault_code_t code, const char *source);
void fault_service_clear(void);
bool fault_service_has_fault(void);
vp_fault_code_t fault_service_get_code(void);
const char *fault_service_code_name(vp_fault_code_t code);

#endif
