#ifndef DISPLAY_SERVICE_H
#define DISPLAY_SERVICE_H

#include "esp_err.h"

esp_err_t display_service_init(void);
esp_err_t display_service_clear(void);
esp_err_t display_service_show_boot_screen(void);

#endif
