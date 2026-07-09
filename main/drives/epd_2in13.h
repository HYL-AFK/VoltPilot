#ifndef EPD_2IN13_H
#define EPD_2IN13_H

#include <stdint.h>
#include "esp_err.h"

esp_err_t epd_2in13_init(void);
esp_err_t epd_2in13_clear(void);
esp_err_t epd_2in13_display_4color(const uint8_t *image);
esp_err_t epd_2in13_display_bw(const uint8_t *black, const uint8_t *red);
esp_err_t epd_2in13_sleep(void);

#endif
