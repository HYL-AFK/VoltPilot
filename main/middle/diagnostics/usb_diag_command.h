#ifndef USB_DIAG_COMMAND_H
#define USB_DIAG_COMMAND_H

typedef enum {
    VP_USB_DIAG_COMMAND_UNKNOWN = 0,
    VP_USB_DIAG_COMMAND_STATUS,
    VP_USB_DIAG_COMMAND_HISTORY,
    VP_USB_DIAG_COMMAND_SUMMARY,
} vp_usb_diag_command_t;

vp_usb_diag_command_t vp_usb_diag_command_parse(const char *line);

#endif
