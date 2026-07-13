#include "ui_service.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_state_service.h"
#include "board_service.h"
#include "fault_service.h"
#include "watchdog_service.h"
#include "vp_board.h"

static const char *TAG = "ui_service";

static void led_task(void *arg)
{
    (void)arg;
    bool blink_on = false;
    (void)watchdog_service_subscribe_current_task("vp_led");

    while (true) {
        vp_app_state_t state = app_state_service_get_state();
        board_status_led_t led_state = BOARD_STATUS_LED_OFF;
        uint32_t period_ms = 250;

        if (fault_service_has_fault() || state == VP_APP_STATE_FAULT) {
            led_state = BOARD_STATUS_LED_RED;
            blink_on = false;
            period_ms = 500;
        } else if (state == VP_APP_STATE_PREPARE) {
            blink_on = !blink_on;
            led_state = blink_on ? BOARD_STATUS_LED_GREEN : BOARD_STATUS_LED_OFF;
        } else if (state == VP_APP_STATE_RUNNING) {
            led_state = BOARD_STATUS_LED_GREEN;
            blink_on = false;
            period_ms = 500;
        } else {
            blink_on = false;
            period_ms = 500;
        }

        (void)board_status_led_set(led_state);
        watchdog_service_feed();
        vTaskDelay(pdMS_TO_TICKS(period_ms));
    }
}

static void post_button_event(vp_app_event_id_t id)
{
    esp_err_t err = app_state_post_event(id, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "按键事件投递失败: %s", esp_err_to_name(err));
    }
}

static void button_task(void *arg)
{
    (void)arg;
    int stable_level = gpio_get_level(VP_PIN_SCREEN_BUTTON);
    int last_sample = stable_level;
    TickType_t last_change = xTaskGetTickCount();
    TickType_t press_start = 0;
    TickType_t last_release = 0;
    int click_count = 0;
    bool long_reported = false;
    (void)watchdog_service_subscribe_current_task("vp_button");

    while (true) {
        TickType_t now = xTaskGetTickCount();
        int sample = gpio_get_level(VP_PIN_SCREEN_BUTTON);

        if (sample != last_sample) {
            last_sample = sample;
            last_change = now;
        }

        if (sample != stable_level &&
            pdTICKS_TO_MS(now - last_change) >= VP_INPUT_DEBOUNCE_MS) {
            stable_level = sample;

            if (stable_level == VP_BUTTON_ACTIVE_LEVEL) {
                press_start = now;
                long_reported = false;
                ESP_LOGI(TAG, "按键按下");
            } else {
                ESP_LOGI(TAG, "按键释放");
                if (!long_reported) {
                    if (click_count == 1 &&
                        pdTICKS_TO_MS(now - last_release) > VP_BUTTON_DOUBLE_CLICK_MS) {
                        click_count = 0;
                    }
                    TickType_t previous_release = last_release;
                    click_count++;
                    last_release = now;
                    if (click_count >= 2 && previous_release != 0 &&
                        pdTICKS_TO_MS(now - previous_release) <= VP_BUTTON_DOUBLE_CLICK_MS) {
                        click_count = 0;
                        ESP_LOGI(TAG, "按键双击");
                        post_button_event(VP_APP_EVENT_BUTTON_DOUBLE);
                    }
                }
            }
        }

        if (stable_level == VP_BUTTON_ACTIVE_LEVEL && !long_reported &&
            pdTICKS_TO_MS(now - press_start) >= VP_BUTTON_LONG_PRESS_MS) {
            long_reported = true;
            click_count = 0;
            ESP_LOGI(TAG, "按键长按");
            post_button_event(VP_APP_EVENT_BUTTON_LONG);
        }

        if (click_count == 1 && stable_level != VP_BUTTON_ACTIVE_LEVEL &&
            pdTICKS_TO_MS(now - last_release) > VP_BUTTON_DOUBLE_CLICK_MS) {
            click_count = 0;
            ESP_LOGI(TAG, "按键单击");
            post_button_event(VP_APP_EVENT_BUTTON_SINGLE);
        }

        watchdog_service_feed();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t ui_service_init(void)
{
    BaseType_t ok = xTaskCreate(led_task, "vp_led", VP_TASK_STACK_NORMAL, NULL,
                                VP_TASK_PRIO_LOW, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "创建 LED 任务失败");

    ok = xTaskCreate(button_task, "vp_button", VP_TASK_STACK_NORMAL, NULL,
                     VP_TASK_PRIO_NORMAL, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "创建按键任务失败");

    (void)board_buzzer_beep(2000, 100);
    ESP_LOGI(TAG, "UI 服务初始化完成");
    return ESP_OK;
}
