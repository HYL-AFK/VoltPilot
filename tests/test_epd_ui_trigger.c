#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "epd_ui_trigger.h"

#define assert(expression) do {                                                    \
    if (!(expression)) {                                                          \
        fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #expression, __FILE__,   \
                __LINE__);                                                        \
        exit(EXIT_FAILURE);                                                       \
    }                                                                             \
} while (0)

static const vp_ui_charging_trigger_config_t s_config = {
    .enter_stable_ms = 3000,
    .exit_stable_ms = 3000,
    .enter_soc_percent = 20,
    .exit_soc_percent = 22,
};

static void test_charge_page_requires_three_seconds_of_stable_charging(void)
{
    vp_ui_charging_trigger_t trigger;
    vp_ui_charging_trigger_reset(&trigger);

    assert(!vp_ui_charging_trigger_update(&trigger, true, 10, true, 0, &s_config));
    assert(!vp_ui_charging_trigger_update(&trigger, true, 10, true, 2999, &s_config));
    assert(vp_ui_charging_trigger_update(&trigger, true, 10, true, 3000, &s_config));
}

static void test_charge_page_has_hysteresis_and_signal_exit_debounce(void)
{
    vp_ui_charging_trigger_t trigger;
    vp_ui_charging_trigger_reset(&trigger);
    assert(!vp_ui_charging_trigger_update(&trigger, true, 10, true, 0, &s_config));
    assert(vp_ui_charging_trigger_update(&trigger, true, 10, true, 3000, &s_config));

    assert(vp_ui_charging_trigger_update(&trigger, true, 10, false, 3001, &s_config));
    assert(vp_ui_charging_trigger_update(&trigger, true, 10, false, 5999, &s_config));
    assert(!vp_ui_charging_trigger_update(&trigger, true, 10, false, 6001, &s_config));

    assert(!vp_ui_charging_trigger_update(&trigger, true, 10, true, 7000, &s_config));
    assert(vp_ui_charging_trigger_update(&trigger, true, 10, true, 10000, &s_config));
    assert(!vp_ui_charging_trigger_update(&trigger, true, 22, true, 10001, &s_config));
}

static void test_charge_page_exits_immediately_when_bms_is_invalid(void)
{
    vp_ui_charging_trigger_t trigger;
    vp_ui_charging_trigger_reset(&trigger);
    assert(!vp_ui_charging_trigger_update(&trigger, true, 10, true, 0, &s_config));
    assert(vp_ui_charging_trigger_update(&trigger, true, 10, true, 3000, &s_config));
    assert(!vp_ui_charging_trigger_update(&trigger, false, 10, true, 3001, &s_config));
}

int main(void)
{
    test_charge_page_requires_three_seconds_of_stable_charging();
    test_charge_page_has_hysteresis_and_signal_exit_debounce();
    test_charge_page_exits_immediately_when_bms_is_invalid();
    puts("epd_ui_trigger tests passed");
    return 0;
}
