#include <assert.h>
#include <stdio.h>

#include "ai_rs485_health.h"

int main(void)
{
    vp_ai_rs485_health_t health;
    vp_ai_rs485_health_init(&health, 3);
    assert(!vp_ai_rs485_health_is_ready(&health));

    assert(vp_ai_rs485_health_note_failure(&health) == VP_AI_RS485_HEALTH_NO_CHANGE);
    assert(vp_ai_rs485_health_note_failure(&health) == VP_AI_RS485_HEALTH_NO_CHANGE);
    assert(vp_ai_rs485_health_note_failure(&health) == VP_AI_RS485_HEALTH_OFFLINE);
    assert(!vp_ai_rs485_health_is_ready(&health));

    assert(vp_ai_rs485_health_note_success(&health) == VP_AI_RS485_HEALTH_RECOVERED);
    assert(vp_ai_rs485_health_is_ready(&health));
    assert(vp_ai_rs485_health_note_failure(&health) == VP_AI_RS485_HEALTH_NO_CHANGE);
    assert(vp_ai_rs485_health_note_failure(&health) == VP_AI_RS485_HEALTH_NO_CHANGE);
    assert(vp_ai_rs485_health_is_ready(&health));
    assert(vp_ai_rs485_health_note_failure(&health) == VP_AI_RS485_HEALTH_OFFLINE);
    assert(!vp_ai_rs485_health_is_ready(&health));

    assert(vp_ai_rs485_health_note_success(&health) == VP_AI_RS485_HEALTH_RECOVERED);
    assert(vp_ai_rs485_health_is_ready(&health));
    puts("ai_rs485 health tests passed");
    return 0;
}
