#include "watchdog_service.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "diag_service.h"
#include "board_service.h"
#include "fault_service.h"
#include "vp_board.h"

#define WATCHDOG_TIMEOUT_MS 8000
#define WATCHDOG_MAX_TASKS 8
#define WATCHDOG_SUPERVISOR_PERIOD_MS 250
#define WATCHDOG_STALE_LIMIT_MS 3000

typedef struct {
    TaskHandle_t handle;
    char name[12];
    uint32_t last_feed_ms;
} watchdog_task_slot_t;

static const char *TAG = "watchdog_service";

static watchdog_task_slot_t s_tasks[WATCHDOG_MAX_TASKS];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_inited;
static bool s_fault_latched;

static uint32_t now_ms(void)
{
    return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
}

static watchdog_task_slot_t *find_slot(TaskHandle_t handle)
{
    for (int i = 0; i < WATCHDOG_MAX_TASKS; i++) {
        if (s_tasks[i].handle == handle) {
            return &s_tasks[i];
        }
    }
    return NULL;
}

static watchdog_task_slot_t *alloc_slot(TaskHandle_t handle, const char *name)
{
    for (int i = 0; i < WATCHDOG_MAX_TASKS; i++) {
        if (s_tasks[i].handle == NULL) {
            s_tasks[i].handle = handle;
            strlcpy(s_tasks[i].name, name != NULL ? name : "task", sizeof(s_tasks[i].name));
            return &s_tasks[i];
        }
    }
    return NULL;
}

static void watchdog_supervisor_task(void *arg)
{
    (void)arg;
    (void)watchdog_service_subscribe_current_task("vp_wdt_sup");

    while (true) {
        uint32_t now = now_ms();
        char stale_name[12] = {0};

        portENTER_CRITICAL(&s_lock);
        for (int i = 0; i < WATCHDOG_MAX_TASKS; i++) {
            if (s_tasks[i].handle != NULL &&
                s_tasks[i].handle != xTaskGetCurrentTaskHandle() &&
                now - s_tasks[i].last_feed_ms > WATCHDOG_STALE_LIMIT_MS) {
                strlcpy(stale_name, s_tasks[i].name, sizeof(stale_name));
                break;
            }
        }
        portEXIT_CRITICAL(&s_lock);

        if (stale_name[0] != '\0' && !s_fault_latched) {
            s_fault_latched = true;
            ESP_LOGE(TAG, "watchdog supervisor detected stale task=%s; forcing outputs off", stale_name);
            (void)board_output_all_off();
            fault_service_raise(VP_FAULT_OUTPUT_ERROR, "WATCHDOG");
        }

        watchdog_service_feed();
        vTaskDelay(pdMS_TO_TICKS(WATCHDOG_SUPERVISOR_PERIOD_MS));
    }
}

esp_err_t watchdog_service_init(void)
{
    esp_task_wdt_config_t config = {
        .timeout_ms = WATCHDOG_TIMEOUT_MS,
        .idle_core_mask = (1U << portNUM_PROCESSORS) - 1U,
        .trigger_panic = true,
    };

    esp_err_t err = esp_task_wdt_reconfigure(&config);
    if (err == ESP_ERR_INVALID_STATE) {
        err = esp_task_wdt_init(&config);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "configure task watchdog failed");

    s_inited = true;
    s_fault_latched = false;
    BaseType_t ok = xTaskCreate(watchdog_supervisor_task, "vp_wdt_sup", VP_TASK_STACK_NORMAL,
                                NULL, VP_TASK_PRIO_HIGH, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create watchdog supervisor failed");
    ESP_LOGI(TAG, "业务看门狗已启用 timeout=%dms trigger_panic=1", WATCHDOG_TIMEOUT_MS);
    return ESP_OK;
}

esp_err_t watchdog_service_subscribe_current_task(const char *name)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    TaskHandle_t handle = xTaskGetCurrentTaskHandle();
    esp_err_t err = esp_task_wdt_add(NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    portENTER_CRITICAL(&s_lock);
    watchdog_task_slot_t *slot = find_slot(handle);
    if (slot == NULL) {
        slot = alloc_slot(handle, name);
    }
    if (slot != NULL) {
        slot->last_feed_ms = now_ms();
    }
    portEXIT_CRITICAL(&s_lock);

    ESP_RETURN_ON_FALSE(slot != NULL, ESP_ERR_NO_MEM, TAG, "watchdog task slot full");
    (void)diag_service_update_watchdog_task(slot->name, slot->last_feed_ms);
    ESP_LOGI(TAG, "订阅看门狗 task=%s", slot->name);
    return ESP_OK;
}

void watchdog_service_feed(void)
{
    if (!s_inited) {
        return;
    }

    TaskHandle_t handle = xTaskGetCurrentTaskHandle();
    uint32_t feed_ms = now_ms();
    char name[12] = {0};

    portENTER_CRITICAL(&s_lock);
    watchdog_task_slot_t *slot = find_slot(handle);
    if (slot != NULL) {
        slot->last_feed_ms = feed_ms;
        strlcpy(name, slot->name, sizeof(name));
    }
    portEXIT_CRITICAL(&s_lock);

    if (slot != NULL) {
        (void)diag_service_update_watchdog_task(name, feed_ms);
    } else {
        return;
    }

    (void)esp_task_wdt_reset();
}
