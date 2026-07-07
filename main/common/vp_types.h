#ifndef VP_TYPES_H
#define VP_TYPES_H

#include <stdint.h>

#define VP_APP_TAG "VoltPilot"
#define VP_EPD_WIDTH 122
#define VP_EPD_HEIGHT 250
#define VP_EPD_WIDTH_BYTES 16
#define VP_EPD_BUFFER_SIZE (VP_EPD_WIDTH_BYTES * VP_EPD_HEIGHT)

typedef enum {
    VP_OK = 0,
    VP_ERR = -1,
    VP_ERR_TIMEOUT = -2,
} vp_status_t;

#endif
