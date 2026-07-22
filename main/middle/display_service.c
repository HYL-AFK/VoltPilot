#include "display_service.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_state_service.h"
#include "bms_service.h"
#include "epd_2in13.h"
#include "epd_ui_renderer.h"
#include "epd_ui_trigger.h"
#include "fault_service.h"
#include "stc_service.h"
#include "vp_board.h"
#include "vp_types.h"

static const char *TAG = "display_service";

static uint8_t s_image[VP_EPD_PLANE_SIZE * 2];
static TaskHandle_t s_display_task;
static vp_ui_charging_trigger_t s_charging_trigger;
static const vp_ui_charging_trigger_config_t s_charging_trigger_config = {
    .enter_stable_ms = VP_UI_CHARGING_ENTER_STABLE_MS,
    .exit_stable_ms = VP_UI_CHARGING_EXIT_STABLE_MS,
    .enter_soc_percent = VP_UI_CHARGING_ENTER_SOC_PERCENT,
    .exit_soc_percent = VP_UI_CHARGING_EXIT_SOC_PERCENT,
};

_Static_assert(sizeof(s_image) == 8000, "SSD1680 black/red image must be 8000 bytes");
_Static_assert(sizeof(s_image) == VP_UI_IMAGE_SIZE, "UI image size must match panel RAM");

#if !VP_EPD_UI_DEMO_DATA
static bool bms_is_actively_charging(const bms_info_t *bms)
{
    if (bms == NULL || bms->charge_mos_state == 0) {
        return false;
    }
#if VP_BMS_CHARGE_CURRENT_IS_POSITIVE
    return bms->current_ma >= VP_BMS_CHARGING_CURRENT_MIN_MA;
#else
    return bms->current_ma <= -VP_BMS_CHARGING_CURRENT_MIN_MA;
#endif
}

static vp_ui_state_t map_app_state(vp_app_state_t state)
{
    switch (state) {
    case VP_APP_STATE_PREPARE: return VP_UI_STATE_PREPARE;
    case VP_APP_STATE_RUNNING: return VP_UI_STATE_RUNNING;
    case VP_APP_STATE_FAULT: return VP_UI_STATE_FAULT;
    case VP_APP_STATE_SHUTDOWN: return VP_UI_STATE_SHUTDOWN;
    default: return VP_UI_STATE_STANDBY;
    }
}
#endif

static vp_ui_snapshot_t take_snapshot(void)
{
#if VP_EPD_UI_DEMO_DATA
    /* 固定演示数据仅用于查看 UI 效果，联调真实设备时将开关改为 0。 */
    return (vp_ui_snapshot_t){
        .bms_valid = true,
        .gear_valid = true,
        .soc_percent = 10,
        .gear = 2,
        .voltage_mv = 55100,
        .current_ma = 0,
        .charge_mos_active = true,
        .state = VP_UI_STATE_RUNNING,
        .fault_code = 0,
    };
#else
    bms_info_t bms = {0};
    stc_info_t stc = {0};
    bool have_bms = bms_service_get_info(&bms);
    bool have_stc = stc_service_get_info(&stc);
    bool bms_valid = have_bms && bms.online && bms.parsed_valid;
    bool charging_signal = bms_is_actively_charging(&bms);
    uint32_t now_ms = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());

    return (vp_ui_snapshot_t){
        .bms_valid = bms_valid,
        .gear_valid = have_stc && stc.online && stc.gear_valid,
        .soc_percent = bms.rsoc_percent,
        .gear = stc.raw_gear,
        .voltage_mv = bms.pack_mv,
        .current_ma = bms.current_ma,
        .charge_mos_active = vp_ui_charging_trigger_update(&s_charging_trigger, bms_valid,
                                                            bms.rsoc_percent, charging_signal,
                                                            now_ms,
                                                            &s_charging_trigger_config),
        .state = map_app_state(app_state_service_get_state()),
        .fault_code = (uint16_t)fault_service_get_code(),
    };
#endif
}

static esp_err_t render_and_refresh(const vp_ui_snapshot_t *snapshot)
{
    vp_epd_ui_render(snapshot, s_image, sizeof(s_image));
    return epd_2in13_display(s_image);
}

static void display_task(void *arg)
{
    (void)arg;
    vp_ui_snapshot_t displayed = take_snapshot();
    bool pending = false;
    TickType_t last_refresh = xTaskGetTickCount();

    while (true) {
        vp_ui_snapshot_t current = take_snapshot();
        bool fault_changed = vp_epd_ui_fault_changed(&displayed, &current);
        bool charging_page_changed = displayed.charge_mos_active != current.charge_mos_active;
        pending = vp_epd_ui_needs_refresh(&displayed, &current);
        bool interval_elapsed = pdTICKS_TO_MS(xTaskGetTickCount() - last_refresh) >= 10000U;

        if (fault_changed || charging_page_changed || (pending && interval_elapsed)) {
            esp_err_t err = render_and_refresh(&current);
            if (err == ESP_OK) {
                displayed = current;
                pending = false;
                last_refresh = xTaskGetTickCount();
                ESP_LOGI(TAG, "客户界面全局刷新完成 SOC=%u gear=%u state=%u fault=%u",
                         current.soc_percent, current.gear, current.state, current.fault_code);
            } else {
                ESP_LOGE(TAG, "客户界面刷新失败: %s", esp_err_to_name(err));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t display_service_init(void)
{
    vp_ui_charging_trigger_reset(&s_charging_trigger);
    esp_err_t err = epd_2in13_init();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "display service initialized");
    }
    return err;
}

esp_err_t display_service_clear(void)
{
    return epd_2in13_clear();
}

esp_err_t display_service_show_boot_screen(void)
{
    vp_ui_snapshot_t snapshot = take_snapshot();
    ESP_RETURN_ON_ERROR(render_and_refresh(&snapshot), TAG, "首次客户界面刷新失败");

    if (s_display_task == NULL) {
        BaseType_t ok = xTaskCreate(display_task, "vp_display", VP_TASK_STACK_LARGE, NULL,
                                    VP_TASK_PRIO_LOW, &s_display_task);
        ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "创建显示任务失败");
    }
    return ESP_OK;
}
