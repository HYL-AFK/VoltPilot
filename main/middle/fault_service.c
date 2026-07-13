#include "fault_service.h"

#include "esp_log.h"

#include "board_service.h"
#include "diag_service.h"

static const char *TAG = "fault_service";

static vp_fault_code_t s_fault_code = VP_FAULT_NONE;
static const char *s_fault_source = "none";

const char *fault_service_code_name(vp_fault_code_t code)
{
    switch (code) {
    case VP_FAULT_NONE:
        return "NONE";
    case VP_FAULT_BMS_TIMEOUT:
        return "BMS_TIMEOUT";
    case VP_FAULT_STC_TIMEOUT:
        return "STC_TIMEOUT";
    case VP_FAULT_ADC_ERROR:
        return "ADC_ERROR";
    case VP_FAULT_OUTPUT_ERROR:
        return "OUTPUT_ERROR";
    case VP_FAULT_USER_REQUEST:
        return "USER_REQUEST";
    case VP_FAULT_STC_VERSION:
        return "STC_VERSION";
    default:
        return "UNKNOWN";
    }
}

esp_err_t fault_service_init(void)
{
    s_fault_code = VP_FAULT_NONE;
    s_fault_source = "boot";
    ESP_LOGI(TAG, "故障服务初始化完成");
    return ESP_OK;
}

void fault_service_raise(vp_fault_code_t code, const char *source)
{
    if (code == VP_FAULT_NONE) {
        return;
    }

    s_fault_code = code;
    s_fault_source = source != NULL ? source : "unknown";
    (void)diag_service_capture_fault_outputs(board_output_get_state());

    /* 故障入口必须先关输出，再做日志、蜂鸣、显示等非安全动作。 */
    (void)board_output_all_off();
    (void)diag_service_record_fault(code, s_fault_source);
    ESP_LOGE(TAG, "故障触发 code=%s source=%s，已关闭所有 EN",
             fault_service_code_name(code), s_fault_source);
}

void fault_service_clear(void)
{
    ESP_LOGW(TAG, "清除故障 code=%s source=%s", fault_service_code_name(s_fault_code), s_fault_source);
    s_fault_code = VP_FAULT_NONE;
    s_fault_source = "clear";
    (void)diag_service_record_fault(VP_FAULT_NONE, s_fault_source);
}

bool fault_service_has_fault(void)
{
    return s_fault_code != VP_FAULT_NONE;
}

vp_fault_code_t fault_service_get_code(void)
{
    return s_fault_code;
}
