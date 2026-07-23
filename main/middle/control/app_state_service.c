#include "app_state_service.h"

#include <inttypes.h>
#include <stdbool.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "board_service.h"
#include "ai_rs485_service.h"
#include "bms_service.h"
#include "diag_service.h"
#include "fault_service.h"
#include "stc_service.h"
#include "watchdog_service.h"
#include "vp_board.h"

typedef struct {
    vp_app_event_id_t id;
    int32_t value;
    uint32_t tick_ms;
} vp_app_event_t;

static const char *TAG = "app_state";

static QueueHandle_t s_event_queue;
static vp_app_state_t s_state = VP_APP_STATE_BOOT;


const char *app_state_name(vp_app_state_t state)
{
    switch (state) {
    case VP_APP_STATE_BOOT:
        return "BOOT";
    case VP_APP_STATE_STANDBY:
        return "STANDBY";
    case VP_APP_STATE_PREPARE:
        return "PREPARE";
    case VP_APP_STATE_RUNNING:
        return "RUNNING";
    case VP_APP_STATE_FAULT:
        return "FAULT";
    case VP_APP_STATE_SHUTDOWN:
        return "SHUTDOWN";
    default:
        return "UNKNOWN";
    }
}

vp_app_state_t app_state_service_get_state(void)
{
    return s_state;
}

esp_err_t app_state_post_event(vp_app_event_id_t id, int32_t value)
{
    if (s_event_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    vp_app_event_t event = {
        .id = id,
        .value = value,
        .tick_ms = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount()),
    };
    return xQueueSend(s_event_queue, &event, 0) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void change_state(vp_app_state_t next)
{
    if (s_state == next) {
        return;
    }

    ESP_LOGI(TAG, "状态切换 %s -> %s", app_state_name(s_state), app_state_name(next));
    s_state = next;
    board_output_state_t out = board_output_get_state();
    vp_fault_code_t fault = fault_service_has_fault() ? fault_service_get_code() : VP_FAULT_NONE;
    (void)diag_service_update_state(s_state, fault, fault_service_code_name(fault), out);
}

static bool start_conditions_ok(void)
{
    bms_info_t bms = {0};
    stc_info_t stc = {0};
    bool bms_ok = bms_service_get_info(&bms);
    bool stc_online = stc_service_get_info(&stc);

    if (!bms_ok) {
        ESP_LOGW(TAG, "启动条件未满足：BMS 还没有解析到有效数据，禁止打开输出");
        return false;
    }

    ESP_LOGI(TAG, "BMS 有效：material=%d cell=%u pack=%" PRId32 "mV current=%" PRId32
             "mA rsoc=%u%%",
             bms.material, bms.cell_count, bms.pack_mv, bms.current_ma, bms.rsoc_percent);

    if (!stc_online) {
        ESP_LOGW(TAG, "启动条件未满足：STC 还没有有效响应，禁止打开输出");
        return false;
    }

    if (!ai_rs485_service_is_ready()) {
        ESP_LOGW(TAG, "启动条件未满足：AI_RS485 四路电源监测离线，禁止启动");
        return false;
    }

    if (!stc_service_version_compatible(&stc)) {
        ESP_LOGE(TAG, "STC version incompatible proto=%u fw=%u.%u.%u hw=%u.%u",
                 stc.protocol_version, stc.firmware_major, stc.firmware_minor,
                 stc.firmware_patch, stc.hardware_major, stc.hardware_minor);
        fault_service_raise(VP_FAULT_STC_VERSION, "STC_VERSION");
        return false;
    }

    /* STC8H 尚未接入时，使用 ESP32 直读三档开关作为挡位来源。 */
    if (!stc.gear_valid) {
        ESP_LOGW(TAG, "启动条件未满足：STC 在线但挡位无效 raw_gear=%u adc_key_raw=%u，禁止打开输出",
                 stc.raw_gear, stc.adc_key_raw);
        return false;
    }


    ESP_LOGI(TAG, "STC 有效：gear=%u adc_key_raw=%u debounce=%ums uptime=%" PRIu32
             "ms status=0x%04X",
             stc.raw_gear, stc.adc_key_raw, stc.debounce_ms, stc.uptime_ms, stc.status_flags);

    /*
     * 挡位已经可以从 STC 获得，但“挡位 -> 24V/36V/48V 输出”的业务映射还未确认。
     * 因此本轮只完成启动条件诊断，不自动打开任何 EN，避免误上电。
    */
    ESP_LOGW(TAG, "启动条件已具备通信基础，但挡位到输出电压的映射未确认，本轮不打开 EN");
#if VP_ENABLE_MOCK_DEVICES
    ESP_LOGW(TAG, "mock 模式：使用虚拟输出继续验证状态机，真实 EN 保持关闭");
    return VP_ENABLE_VIRTUAL_OUTPUT != 0;
#endif
    return false;
}

static void handle_button_single(void)
{
    if (s_state == VP_APP_STATE_STANDBY) {
        change_state(VP_APP_STATE_PREPARE);
        (void)board_buzzer_beep(1800, 80);
        ESP_LOGI(TAG, "单击：进入启动准备，等待双击确认");
    } else if (s_state == VP_APP_STATE_PREPARE) {
        change_state(VP_APP_STATE_STANDBY);
        (void)board_buzzer_beep(1200, 60);
        ESP_LOGI(TAG, "单击：取消启动准备，回到待机");
    }
}

static void handle_button_double(void)
{
    if (s_state != VP_APP_STATE_STANDBY && s_state != VP_APP_STATE_PREPARE) {
        ESP_LOGW(TAG, "双击被忽略：当前状态=%s", app_state_name(s_state));
        return;
    }

    change_state(VP_APP_STATE_PREPARE);
    if (!start_conditions_ok()) {
        (void)board_buzzer_beep(800, 120);
        return;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(board_output_all_off());
    vTaskDelay(pdMS_TO_TICKS(VP_OUTPUT_INTERLOCK_DELAY_MS));
#if VP_ENABLE_MOCK_DEVICES && VP_ENABLE_VIRTUAL_OUTPUT
    stc_info_t stc = {0};
    (void)stc_service_get_info(&stc);
    (void)board_virtual_output_request(stc.raw_gear);
    (void)board_buzzer_beep(2400, 180);
    change_state(VP_APP_STATE_RUNNING);
    return;
#endif
    ESP_LOGW(TAG, "目标输出映射尚未确认，本轮不打开 EN");
}

static void handle_button_long(void)
{
    ESP_LOGW(TAG, "长按：执行安全关断");
    (void)board_output_all_off();
    (void)board_buzzer_beep(1000, 150);
    if (s_state == VP_APP_STATE_FAULT) {
        fault_service_clear();
    }
    change_state(VP_APP_STATE_STANDBY);
}

static void handle_timeout_event(vp_app_event_id_t id)
{
    if (s_state == VP_APP_STATE_RUNNING) {
        vp_fault_code_t code = id == VP_APP_EVENT_BMS_TIMEOUT ? VP_FAULT_BMS_TIMEOUT : VP_FAULT_STC_TIMEOUT;
        fault_service_raise(code, id == VP_APP_EVENT_BMS_TIMEOUT ? "BMS" : "STC");
        change_state(VP_APP_STATE_FAULT);
    } else {
        ESP_LOGW(TAG, "通信超时占位事件 id=%d，当前未运行，仅记录", id);
    }
}

static void app_state_task(void *arg)
{
    (void)arg;
    TickType_t last_health = xTaskGetTickCount();
#if VP_ENABLE_MOCK_DEVICES && VP_MOCK_FORCE_FAULT
    TickType_t boot_tick = last_health;
    bool mock_fault_triggered = false;
#endif
    (void)watchdog_service_subscribe_current_task("vp_state");

    change_state(VP_APP_STATE_STANDBY);

    while (true) {
        vp_app_event_t event;
        if (xQueueReceive(s_event_queue, &event, pdMS_TO_TICKS(200)) == pdPASS) {
            switch (event.id) {
            case VP_APP_EVENT_BUTTON_SINGLE:
                handle_button_single();
                break;
            case VP_APP_EVENT_BUTTON_DOUBLE:
                handle_button_double();
                break;
            case VP_APP_EVENT_BUTTON_LONG:
                handle_button_long();
                break;
            case VP_APP_EVENT_BMS_RX:
            case VP_APP_EVENT_ADC_UPDATE:
                break;
            case VP_APP_EVENT_STC_RX:
                {
                    stc_info_t stc = {0};
                    (void)stc_service_get_info(&stc);
                    if (!stc_service_version_compatible(&stc)) {
                        fault_service_raise(VP_FAULT_STC_VERSION, "STC_VERSION");
                        change_state(VP_APP_STATE_FAULT);
                    }
                }
                break;
            case VP_APP_EVENT_BMS_TIMEOUT:
            case VP_APP_EVENT_STC_TIMEOUT:
                handle_timeout_event(event.id);
                break;
            case VP_APP_EVENT_AI_RS485_OFFLINE:
                if (s_state == VP_APP_STATE_RUNNING) {
                    fault_service_raise(VP_FAULT_AI_RS485_TIMEOUT, "AI_RS485");
                    change_state(VP_APP_STATE_FAULT);
                } else {
                    ESP_LOGW(TAG, "AI_RS485 离线：待机告警，运行启动将被禁止");
                    (void)diag_service_record_event(VP_DIAG_EVENT_AI_RS485_OFFLINE);
                }
                break;
            case VP_APP_EVENT_AI_RS485_RECOVERED:
                ESP_LOGI(TAG, "AI_RS485 已恢复；如处于FAULT仍需长按人工清除");
                (void)diag_service_record_event(VP_DIAG_EVENT_AI_RS485_RECOVERED);
                break;
            case VP_APP_EVENT_FAULT:
                fault_service_raise((vp_fault_code_t)event.value, "event");
                change_state(VP_APP_STATE_FAULT);
                break;
            default:
                ESP_LOGW(TAG, "未知事件 id=%d value=%" PRId32, event.id, event.value);
                break;
            }
        }

#if VP_ENABLE_MOCK_DEVICES && VP_MOCK_FORCE_FAULT
        if (!mock_fault_triggered &&
            pdTICKS_TO_MS(xTaskGetTickCount() - boot_tick) >= VP_MOCK_FORCE_FAULT_DELAY_MS) {
            mock_fault_triggered = true;
            fault_service_raise(VP_FAULT_USER_REQUEST, "MOCK_FORCE_FAULT");
            change_state(VP_APP_STATE_FAULT);
        }
#endif

        if (pdTICKS_TO_MS(xTaskGetTickCount() - last_health) >= 5000) {
            board_output_state_t out = board_output_get_state();
            ESP_LOGI(TAG, "心跳 state=%s EN24=%d EN36=%d EN48=%d fault=%s",
                     app_state_name(s_state), out.en_24v, out.en_36v, out.en_48v,
                     fault_service_has_fault() ? fault_service_code_name(fault_service_get_code()) : "NONE");
            last_health = xTaskGetTickCount();
        }
        watchdog_service_feed();
    }
}

esp_err_t app_state_service_init(void)
{
    s_event_queue = xQueueCreate(16, sizeof(vp_app_event_t));
    ESP_RETURN_ON_FALSE(s_event_queue != NULL, ESP_ERR_NO_MEM, TAG, "创建状态机队列失败");

    BaseType_t ok = xTaskCreate(app_state_task, "vp_state", VP_TASK_STACK_NORMAL, NULL,
                                VP_TASK_PRIO_NORMAL, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "创建状态机任务失败");

    ESP_LOGI(TAG, "状态机服务初始化完成");
    return ESP_OK;
}
