#ifndef ADC_SERVICE_H
#define ADC_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    VP_ADC_INPUT_24V = 0,
    VP_ADC_INPUT_5V,
    VP_ADC_INPUT_36V,
    VP_ADC_INPUT_48V,
    VP_ADC_INPUT_COUNT,
} vp_adc_input_id_t;

typedef struct {
    uint32_t raw;
    uint32_t adc_mv;
    uint32_t bus_mv;
    uint32_t samples;
} vp_adc_channel_value_t;

typedef struct {
    vp_adc_channel_value_t channel[VP_ADC_INPUT_COUNT];
    uint32_t update_ms;
    bool valid;
} vp_adc_snapshot_t;

esp_err_t adc_service_init(void);
bool adc_service_get_snapshot(vp_adc_snapshot_t *out_snapshot);

#endif
