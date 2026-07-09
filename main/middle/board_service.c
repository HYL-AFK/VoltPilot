#include "board_service.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "diag_service.h"
#include "vp_board.h"

static const char *TAG = "board_service";

static board_output_state_t s_output_state;
static bool s_board_inited;

static esp_err_t board_set_output(gpio_num_t pin, bool *state, bool enable)
{
    *state = enable;
    esp_err_t err = gpio_set_level(pin, enable ? VP_OUTPUT_ENABLE_ACTIVE_LEVEL : VP_OUTPUT_ENABLE_INACTIVE_LEVEL);
    if (err == ESP_OK) {
        (void)diag_service_update_outputs(s_output_state);
    }
    return err;
}

esp_err_t board_output_set_24v(bool enable)
{
    return board_set_output(VP_OUT_PIN_EN_24V, &s_output_state.en_24v, enable);
}

esp_err_t board_output_set_36v(bool enable)
{
    return board_set_output(VP_OUT_PIN_EN_36V, &s_output_state.en_36v, enable);
}

esp_err_t board_output_set_48v(bool enable)
{
    return board_set_output(VP_OUT_PIN_EN_48V, &s_output_state.en_48v, enable);
}

esp_err_t board_output_all_off(void)
{
    /* 安全优先：任意故障入口都调用这里，确保三个输出使能全部拉到关闭电平。 */
    ESP_RETURN_ON_ERROR(board_output_set_24v(false), TAG, "关闭 24V 输出失败");
    ESP_RETURN_ON_ERROR(board_output_set_36v(false), TAG, "关闭 36V 输出失败");
    ESP_RETURN_ON_ERROR(board_output_set_48v(false), TAG, "关闭 48V 输出失败");
    return ESP_OK;
}

board_output_state_t board_output_get_state(void)
{
    return s_output_state;
}

esp_err_t board_sys_led_set(bool on)
{
    return gpio_set_level(VP_PIN_SYS_LED, on ? VP_SYS_LED_ACTIVE_LEVEL : !VP_SYS_LED_ACTIVE_LEVEL);
}

esp_err_t board_buzzer_beep(uint32_t freq_hz, uint32_t duration_ms)
{
    if (!s_board_inited || freq_hz == 0 || duration_ms == 0) {
        return ledc_stop(VP_BUZZER_LEDC_MODE, VP_BUZZER_LEDC_CHANNEL, 0);
    }

    ESP_RETURN_ON_ERROR(ledc_set_freq(VP_BUZZER_LEDC_MODE, VP_BUZZER_LEDC_TIMER, freq_hz),
                        TAG, "设置蜂鸣器频率失败");
    ESP_RETURN_ON_ERROR(ledc_set_duty(VP_BUZZER_LEDC_MODE, VP_BUZZER_LEDC_CHANNEL, VP_BUZZER_LEDC_DUTY_ON),
                        TAG, "设置蜂鸣器占空比失败");
    ESP_RETURN_ON_ERROR(ledc_update_duty(VP_BUZZER_LEDC_MODE, VP_BUZZER_LEDC_CHANNEL),
                        TAG, "更新蜂鸣器占空比失败");
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    return ledc_stop(VP_BUZZER_LEDC_MODE, VP_BUZZER_LEDC_CHANNEL, 0);
}

static esp_err_t board_gpio_init(void)
{
    gpio_config_t output_cfg = {
        .pin_bit_mask = (1ULL << VP_OUT_PIN_EN_24V) |
                        (1ULL << VP_OUT_PIN_EN_36V) |
                        (1ULL << VP_OUT_PIN_EN_48V) |
                        (1ULL << VP_PIN_SYS_LED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output_cfg), TAG, "配置输出 GPIO 失败");

    ESP_RETURN_ON_ERROR(board_output_all_off(), TAG, "默认关闭输出失败");
    ESP_RETURN_ON_ERROR(board_sys_led_set(false), TAG, "关闭系统灯失败");

    gpio_config_t input_cfg = {
        .pin_bit_mask = (1ULL << VP_PIN_SCREEN_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&input_cfg), TAG, "配置按键 GPIO 失败");
    return ESP_OK;
}

static esp_err_t board_buzzer_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = VP_BUZZER_LEDC_MODE,
        .timer_num = VP_BUZZER_LEDC_TIMER,
        .duty_resolution = VP_BUZZER_LEDC_DUTY_RES,
        .freq_hz = 2000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "配置蜂鸣器定时器失败");

    ledc_channel_config_t channel_cfg = {
        .gpio_num = VP_PIN_BUZZER_PWM,
        .speed_mode = VP_BUZZER_LEDC_MODE,
        .channel = VP_BUZZER_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = VP_BUZZER_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&channel_cfg);
}

esp_err_t board_service_init(void)
{
    ESP_LOGI(TAG,
             "板级引脚: ADC24=%d ADC5=%d ADC36=%d ADC48=%d RS485_RX=%d RS485_TX=%d "
             "EN48=%d EN36=%d EN24=%d STC_TX=%d STC_RX=%d BUTTON=%d BUZZER=%d SYS_LED=%d",
             VP_ADC_PIN_24V, VP_ADC_PIN_5V, VP_ADC_PIN_36V, VP_ADC_PIN_48V,
             VP_BMS_PIN_RX, VP_BMS_PIN_TX,
             VP_OUT_PIN_EN_48V, VP_OUT_PIN_EN_36V, VP_OUT_PIN_EN_24V,
             VP_STC_PIN_TX, VP_STC_PIN_RX,
             VP_PIN_SCREEN_BUTTON, VP_PIN_BUZZER_PWM, VP_PIN_SYS_LED);

    ESP_RETURN_ON_ERROR(board_gpio_init(), TAG, "GPIO 初始化失败");
    ESP_RETURN_ON_ERROR(board_buzzer_init(), TAG, "蜂鸣器初始化失败");
    s_board_inited = true;

    ESP_LOGI(TAG, "输出默认关闭，EN 高有效");
    ESP_LOGI(TAG, "板级初始化完成");
    return ESP_OK;
}
