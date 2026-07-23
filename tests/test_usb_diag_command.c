#include <assert.h>
#include <stdio.h>

#include "usb_diag_command.h"

int main(void)
{
    assert(vp_usb_diag_command_parse("VP STATUS\r\n") == VP_USB_DIAG_COMMAND_STATUS);
    assert(vp_usb_diag_command_parse("VP HISTORY\n") == VP_USB_DIAG_COMMAND_HISTORY);
    assert(vp_usb_diag_command_parse("VP SUMMARY") == VP_USB_DIAG_COMMAND_SUMMARY);
    assert(vp_usb_diag_command_parse("VP CLEAR") == VP_USB_DIAG_COMMAND_UNKNOWN);
    assert(vp_usb_diag_command_parse("VP HISTORY extra") == VP_USB_DIAG_COMMAND_UNKNOWN);
    puts("usb diagnostic command tests passed");
    return 0;
}
