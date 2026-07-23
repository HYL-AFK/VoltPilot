#include "usb_diag_service.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ai_rs485_service.h"
#include "diag_service.h"
#include "usb_diag_command.h"
#include "vp_board.h"

#define VP_USB_DIAG_LINE_MAX 64U
#define VP_USB_DIAG_TX_TIMEOUT_MS 1000U

static const char *TAG = "usb_diag";

static void usb_write_text(const char *text)
{
    if (text == NULL || !usb_serial_jtag_is_connected()) {
        return;
    }
    size_t remaining = strlen(text);
    const char *cursor = text;
    while (remaining > 0) {
        int written = usb_serial_jtag_write_bytes(cursor, remaining,
                                                   pdMS_TO_TICKS(VP_USB_DIAG_TX_TIMEOUT_MS));
        if (written <= 0) {
            return;
        }
        cursor += written;
        remaining -= (size_t)written;
    }
}

static void usb_writef(const char *format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    usb_write_text(line);
}

static uint16_t read_user_u16(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8) | data[1];
}

static void export_status(void)
{
    vp_diag_registers_t diag = {0};
    ai_rs485_snapshot_t ai = {0};
    if (diag_service_get_snapshot(&diag) != ESP_OK) {
        usb_write_text("VP,ERROR,STATUS,UNAVAILABLE\r\n");
        return;
    }
    (void)ai_rs485_service_get_snapshot(&ai);
    usb_write_text("VP,BEGIN,STATUS\r\n");
    usb_write_text("boot_count,app_state,fault_code,bms_online,stc_online,ai_online,ai_failures,en24,en36,en48\r\n");
    usb_writef("STATUS,%" PRIu32 ",%u,%u,%u,%u,%u,%u,%u,%u,%u\r\n",
               diag.boot_count, diag.app_state, diag.fault_code, diag.bms_online,
               diag.stc_online, ai.online, ai.consecutive_failures, diag.en_24v,
               diag.en_36v, diag.en_48v);
    usb_write_text("VP,END,STATUS\r\n");
}

static void export_history(void)
{
    size_t count = diag_service_runtime_history_count();
    usb_writef("VP,BEGIN,HISTORY,count=%u\r\n", (unsigned)count);
    usb_write_text("sequence,uptime_ms,type,event_code,app_state,fault_code,gear,soc_percent,pack_mv,current_ma,status_flags,ai24_raw,ai36_raw,ai48_raw,ai5_raw,adc24_mv,adc5_mv,adc36_mv,adc48_mv\r\n");
    for (size_t index = 0; index < count; index++) {
        vp_runtime_history_record_t record = {0};
        esp_err_t err = diag_service_get_runtime_history(index, &record);
        if (err != ESP_OK) {
            usb_writef("VP,ERROR,HISTORY,index=%u,err=%s\r\n", (unsigned)index,
                       esp_err_to_name(err));
            continue;
        }
        usb_writef("%" PRIu32 ",%" PRIu32 ",%u,%" PRIu32 ",%u,%u,%u,%u,%" PRId32 ",%" PRId32 ",0x%04X,%u,%u,%u,%u,%u,%u,%u,%u\r\n",
                   record.sequence, record.uptime_ms, record.record_type, record.event_code,
                   record.app_state, record.fault_code, record.gear, record.soc_percent,
                   record.pack_mv, record.current_ma, record.status_flags,
                   read_user_u16(&record.user_data[0]), read_user_u16(&record.user_data[2]),
                   read_user_u16(&record.user_data[4]), read_user_u16(&record.user_data[6]),
                   read_user_u16(&record.user_data[8]), read_user_u16(&record.user_data[10]),
                   read_user_u16(&record.user_data[12]), read_user_u16(&record.user_data[14]));
    }
    usb_write_text("VP,END,HISTORY\r\n");
}

static void export_summary(void)
{
    vp_runtime_history_summary_t summary = {0};
    usb_write_text("VP,BEGIN,SUMMARY\r\n");
    if (diag_service_get_runtime_summary(&summary) == ESP_OK) {
        usb_write_text("window_start_ms,window_end_ms,sample_count,min_pack_mv,max_pack_mv,max_temperature_c,min_soc_percent,event_count\r\n");
        usb_writef("SUMMARY,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRId32 ",%" PRId32 ",%d,%u,%" PRIu32 "\r\n",
                   summary.window_start_ms, summary.window_end_ms, summary.sample_count,
                   summary.min_pack_mv, summary.max_pack_mv, summary.max_temperature_c,
                   summary.min_soc_percent, summary.event_count);
    } else {
        usb_write_text("SUMMARY,UNAVAILABLE\r\n");
    }
    usb_write_text("VP,END,SUMMARY\r\n");
}

static void process_command(const char *line)
{
    switch (vp_usb_diag_command_parse(line)) {
    case VP_USB_DIAG_COMMAND_STATUS:
        export_status();
        break;
    case VP_USB_DIAG_COMMAND_HISTORY:
        export_history();
        break;
    case VP_USB_DIAG_COMMAND_SUMMARY:
        export_summary();
        break;
    default:
        usb_write_text("VP,ERROR,UNKNOWN_COMMAND\r\n");
        break;
    }
}

static void usb_diag_task(void *arg)
{
    (void)arg;
    char line[VP_USB_DIAG_LINE_MAX] = {0};
    size_t length = 0;
    while (true) {
        uint8_t byte = 0;
        int read = usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(100));
        if (read <= 0) {
            continue;
        }
        if (byte == '\r' || byte == '\n') {
            if (length > 0) {
                line[length] = '\0';
                process_command(line);
                length = 0;
            }
        } else if (length < sizeof(line) - 1U) {
            line[length++] = (char)byte;
        } else {
            length = 0;
            usb_write_text("VP,ERROR,COMMAND_TOO_LONG\r\n");
        }
    }
}

esp_err_t usb_diag_service_init(void)
{
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&config), TAG,
                            "install USB Serial/JTAG driver failed");
        usb_serial_jtag_vfs_use_driver();
    }
    BaseType_t ok = xTaskCreate(usb_diag_task, "vp_usb_diag", VP_TASK_STACK_NORMAL, NULL,
                                VP_TASK_PRIO_LOW, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create USB diagnostic task failed");
    ESP_LOGI(TAG, "USB diagnostics ready: VP STATUS | VP HISTORY | VP SUMMARY");
    return ESP_OK;
}
