#include "usb_diag_command.h"

#include <stdbool.h>
#include <string.h>

static bool command_equals(const char *line, const char *command)
{
    size_t length = strlen(command);
    return strncmp(line, command, length) == 0 &&
           (line[length] == '\0' || line[length] == '\r' || line[length] == '\n');
}

vp_usb_diag_command_t vp_usb_diag_command_parse(const char *line)
{
    if (line == NULL) {
        return VP_USB_DIAG_COMMAND_UNKNOWN;
    }
    if (command_equals(line, "VP STATUS")) {
        return VP_USB_DIAG_COMMAND_STATUS;
    }
    if (command_equals(line, "VP HISTORY")) {
        return VP_USB_DIAG_COMMAND_HISTORY;
    }
    if (command_equals(line, "VP SUMMARY")) {
        return VP_USB_DIAG_COMMAND_SUMMARY;
    }
    return VP_USB_DIAG_COMMAND_UNKNOWN;
}
