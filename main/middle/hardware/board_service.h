#ifndef BOARD_SERVICE_H
#define BOARD_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool en_24v;
    bool en_36v;
    bool en_48v;
} board_output_state_t;

typedef enum {
    BOARD_STATUS_LED_OFF = 0,
    BOARD_STATUS_LED_GREEN,
    BOARD_STATUS_LED_RED,
} board_status_led_t;

esp_err_t board_service_init(void);

esp_err_t board_output_set_24v(bool enable);
esp_err_t board_output_set_36v(bool enable);
esp_err_t board_output_set_48v(bool enable);
esp_err_t board_output_all_off(void);
board_output_state_t board_output_get_state(void);
esp_err_t board_virtual_output_request(uint8_t gear);

esp_err_t board_buzzer_beep(uint32_t freq_hz, uint32_t duration_ms);
esp_err_t board_status_led_set(board_status_led_t state);

#endif
