#include "app_start.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "adc_service.h"
#include "ai_rs485_service.h"
#include "app_state_service.h"
#include "bms_service.h"
#include "board_service.h"
#include "diag_service.h"
#include "display_service.h"
#include "fault_service.h"
#include "stc_service.h"
#include "ui_service.h"
#include "watchdog_service.h"
#include "vp_board.h"
#include "vp_types.h"

static const char *TAG = VP_APP_TAG;

void app_main_start(void)
{
    ESP_LOGI(TAG, "VoltPilot boot start");

    esp_err_t err = board_service_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board service init failed: %s", esp_err_to_name(err));
        return;
    }

    err = fault_service_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fault service init failed: %s", esp_err_to_name(err));
        return;
    }

    err = diag_service_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "diag service init failed: %s", esp_err_to_name(err));
        return;
    }

    err = watchdog_service_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "watchdog service init failed: %s", esp_err_to_name(err));
        (void)board_output_all_off();
        return;
    }

#if VP_ENABLE_STATE_SERVICE
    err = app_state_service_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "state service init failed: %s", esp_err_to_name(err));
        (void)board_output_all_off();
        return;
    }
#endif

#if VP_ENABLE_ADC_SERVICE
    err = adc_service_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc service init failed: %s", esp_err_to_name(err));
        (void)board_output_all_off();
        return;
    }
#endif

#if VP_ENABLE_BMS_SERVICE
    err = bms_service_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bms service init failed: %s", esp_err_to_name(err));
        (void)board_output_all_off();
        return;
    }
#endif

#if VP_ENABLE_STC_SERVICE
    err = stc_service_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stc service init failed: %s", esp_err_to_name(err));
        (void)board_output_all_off();
        return;
    }
#endif

#if VP_ENABLE_UI_SERVICE
    err = ui_service_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ui service init failed: %s", esp_err_to_name(err));
    }
#endif

    err = ai_rs485_service_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AI RS485 init failed: %s", esp_err_to_name(err));
    }

#if VP_ENABLE_DISPLAY
    err = display_service_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
    } else {
        err = display_service_show_boot_screen();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "boot screen failed: %s", esp_err_to_name(err));
        }
    }
#else
    ESP_LOGI(TAG, "display disabled");
#endif

    ESP_LOGI(TAG, "VoltPilot services started");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
