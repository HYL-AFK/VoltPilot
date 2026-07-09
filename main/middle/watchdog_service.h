#ifndef WATCHDOG_SERVICE_H
#define WATCHDOG_SERVICE_H

#include "esp_err.h"

esp_err_t watchdog_service_init(void);
esp_err_t watchdog_service_subscribe_current_task(const char *name);
void watchdog_service_feed(void);

#endif

