#ifndef USB_DIAG_SERVICE_H
#define USB_DIAG_SERVICE_H

#include "esp_err.h"

/* 通过 ESP32-S3 USB Serial/JTAG 提供只读诊断 CSV 导出。 */
esp_err_t usb_diag_service_init(void);

#endif
