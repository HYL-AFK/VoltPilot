#include "app_start.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display_service.h"
#include "vp_types.h"

static const char *TAG = VP_APP_TAG;

void app_main_start(void)
{
    ESP_LOGI(TAG, "VoltPilot boot start");

    esp_err_t err = display_service_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
    } else {
        err = display_service_clear();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "display clear failed: %s", esp_err_to_name(err));
        }

        err = display_service_show_boot_screen();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "boot screen failed: %s", esp_err_to_name(err));
        }
    }

    while (true) {
        ESP_LOGI(TAG, "alive");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
