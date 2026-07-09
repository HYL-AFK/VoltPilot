#include "adc_service.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_adc/adc_continuous.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/adc_types.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#include "app_state_service.h"
#include "diag_service.h"
#include "watchdog_service.h"
#include "vp_board.h"

#define VP_ADC_READ_LEN 256

#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2
#define VP_ADC_OUTPUT_TYPE ADC_DIGI_OUTPUT_FORMAT_TYPE1
#define VP_ADC_GET_CHANNEL(p_data) ((p_data)->type1.channel)
#define VP_ADC_GET_DATA(p_data) ((p_data)->type1.data)
#else
#define VP_ADC_OUTPUT_TYPE ADC_DIGI_OUTPUT_FORMAT_TYPE2
#define VP_ADC_GET_CHANNEL(p_data) ((p_data)->type2.channel)
#define VP_ADC_GET_DATA(p_data) ((p_data)->type2.data)
#endif

typedef struct {
    const char *name;
    gpio_num_t gpio;
    uint32_t ratio_num;
    uint32_t ratio_den;
    adc_unit_t unit;
    adc_channel_t channel;
} adc_input_cfg_t;

static const char *TAG = "adc_service";

static adc_input_cfg_t s_inputs[VP_ADC_INPUT_COUNT] = {
    [VP_ADC_INPUT_24V] = {.name = "24V", .gpio = VP_ADC_PIN_24V, .ratio_num = VP_ADC_RATIO_24V_NUM, .ratio_den = VP_ADC_RATIO_24V_DEN},
    [VP_ADC_INPUT_5V] = {.name = "5V", .gpio = VP_ADC_PIN_5V, .ratio_num = VP_ADC_RATIO_5V_NUM, .ratio_den = VP_ADC_RATIO_5V_DEN},
    [VP_ADC_INPUT_36V] = {.name = "36V", .gpio = VP_ADC_PIN_36V, .ratio_num = VP_ADC_RATIO_36V_NUM, .ratio_den = VP_ADC_RATIO_36V_DEN},
    [VP_ADC_INPUT_48V] = {.name = "48V", .gpio = VP_ADC_PIN_48V, .ratio_num = VP_ADC_RATIO_48V_NUM, .ratio_den = VP_ADC_RATIO_48V_DEN},
};

static adc_continuous_handle_t s_adc_handle;
static vp_adc_snapshot_t s_snapshot;
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t raw_to_adc_mv(uint32_t raw)
{
    return (raw * VP_ADC_APPROX_FULL_SCALE_MV) / ((1 << SOC_ADC_DIGI_MAX_BITWIDTH) - 1);
}

static uint32_t adc_mv_to_bus_mv(uint32_t adc_mv, const adc_input_cfg_t *input)
{
    if (input->ratio_den == 0) {
        return adc_mv;
    }
    return (adc_mv * input->ratio_num) / input->ratio_den;
}

static int input_index_from_channel(uint32_t channel)
{
    for (int i = 0; i < VP_ADC_INPUT_COUNT; i++) {
        if ((uint32_t)s_inputs[i].channel == channel) {
            return i;
        }
    }
    return -1;
}

bool adc_service_get_snapshot(vp_adc_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return false;
    }

    taskENTER_CRITICAL(&s_snapshot_lock);
    *out_snapshot = s_snapshot;
    taskEXIT_CRITICAL(&s_snapshot_lock);
    return out_snapshot->valid;
}

static esp_err_t adc_dma_init(void)
{
    adc_digi_pattern_config_t patterns[VP_ADC_INPUT_COUNT] = {0};

    for (int i = 0; i < VP_ADC_INPUT_COUNT; i++) {
        ESP_RETURN_ON_ERROR(adc_continuous_io_to_channel(s_inputs[i].gpio,
                                                         &s_inputs[i].unit,
                                                         &s_inputs[i].channel),
                            TAG, "GPIO 转 ADC 通道失败");
        ESP_RETURN_ON_FALSE(s_inputs[i].unit == ADC_UNIT_1, ESP_ERR_INVALID_ARG,
                            TAG, "%s 不在 ADC1，连续 DMA 暂不支持混用", s_inputs[i].name);

        patterns[i].atten = ADC_ATTEN_DB_12;
        patterns[i].channel = s_inputs[i].channel & 0x7;
        patterns[i].unit = s_inputs[i].unit;
        patterns[i].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;
        ESP_LOGI(TAG, "ADC_%s GPIO=%d unit=%d channel=%d",
                 s_inputs[i].name, s_inputs[i].gpio, s_inputs[i].unit, s_inputs[i].channel);
    }

    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = 1024,
        .conv_frame_size = VP_ADC_READ_LEN,
    };
    ESP_RETURN_ON_ERROR(adc_continuous_new_handle(&handle_cfg, &s_adc_handle),
                        TAG, "创建 ADC DMA 句柄失败");

    adc_continuous_config_t config = {
        .sample_freq_hz = VP_ADC_SAMPLE_FREQ_HZ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = VP_ADC_OUTPUT_TYPE,
        .pattern_num = VP_ADC_INPUT_COUNT,
        .adc_pattern = patterns,
    };
    ESP_RETURN_ON_ERROR(adc_continuous_config(s_adc_handle, &config), TAG, "配置 ADC DMA 失败");
    ESP_RETURN_ON_ERROR(adc_continuous_start(s_adc_handle), TAG, "启动 ADC DMA 失败");
    ESP_LOGI(TAG, "ADC DMA 启动，sample=%dHz", VP_ADC_SAMPLE_FREQ_HZ);
    return ESP_OK;
}

static void adc_service_task(void *arg)
{
    (void)arg;
    uint8_t result[VP_ADC_READ_LEN];
    uint64_t sum[VP_ADC_INPUT_COUNT] = {0};
    uint32_t count[VP_ADC_INPUT_COUNT] = {0};
    TickType_t last_log = xTaskGetTickCount();
    (void)watchdog_service_subscribe_current_task("vp_adc");

    while (true) {
        uint32_t ret_num = 0;
        esp_err_t err = adc_continuous_read(s_adc_handle, result, sizeof(result), &ret_num, 100);

        if (err == ESP_OK) {
            for (uint32_t i = 0; i + SOC_ADC_DIGI_RESULT_BYTES <= ret_num; i += SOC_ADC_DIGI_RESULT_BYTES) {
                adc_digi_output_data_t *sample = (adc_digi_output_data_t *)&result[i];
                int index = input_index_from_channel(VP_ADC_GET_CHANNEL(sample));
                if (index >= 0) {
                    sum[index] += VP_ADC_GET_DATA(sample);
                    count[index]++;
                }
            }
        } else if (err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "ADC 读取异常: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        if (pdTICKS_TO_MS(xTaskGetTickCount() - last_log) >= 1000) {
            vp_adc_snapshot_t next = {
                .update_ms = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount()),
                .valid = true,
            };
            char line[224];
            int offset = 0;

            for (int i = 0; i < VP_ADC_INPUT_COUNT; i++) {
                uint32_t avg = count[i] == 0 ? 0 : (uint32_t)(sum[i] / count[i]);
                uint32_t adc_mv = raw_to_adc_mv(avg);
                uint32_t bus_mv = adc_mv_to_bus_mv(adc_mv, &s_inputs[i]);

                next.channel[i].raw = avg;
                next.channel[i].adc_mv = adc_mv;
                next.channel[i].bus_mv = bus_mv;
                next.channel[i].samples = count[i];

                offset += snprintf(line + offset, sizeof(line) - offset,
                                   "%s raw=%" PRIu32 " adc=%" PRIu32 "mV bus=%" PRIu32 "mV%s",
                                   s_inputs[i].name, avg, adc_mv, bus_mv,
                                   i == VP_ADC_INPUT_COUNT - 1 ? "" : " | ");
                sum[i] = 0;
                count[i] = 0;
            }

            taskENTER_CRITICAL(&s_snapshot_lock);
            s_snapshot = next;
            taskEXIT_CRITICAL(&s_snapshot_lock);

            line[sizeof(line) - 1] = '\0';
            ESP_LOGI(TAG, "ADC采样 %s", line);
            (void)diag_service_update_adc(&s_snapshot);
            (void)app_state_post_event(VP_APP_EVENT_ADC_UPDATE, 0);
            last_log = xTaskGetTickCount();
        }
        watchdog_service_feed();
    }
}

esp_err_t adc_service_init(void)
{
    ESP_RETURN_ON_ERROR(adc_dma_init(), TAG, "ADC DMA 初始化失败");
    BaseType_t ok = xTaskCreate(adc_service_task, "vp_adc", VP_TASK_STACK_LARGE, NULL,
                                VP_TASK_PRIO_NORMAL, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "创建 ADC 任务失败");
    ESP_LOGI(TAG, "ADC 服务初始化完成");
    return ESP_OK;
}
