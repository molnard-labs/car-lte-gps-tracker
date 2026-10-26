#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tracker_output.h"
#include "output_test_platform.h"
static int s_level, s_gpio_error;
static bool s_held;
int gpio_set_level(int gpio, int value) { assert(gpio == 23); if (s_gpio_error) return -1; s_level = value; return 0; }
int gpio_get_level(int gpio) { assert(gpio == 23); return s_level; }
int gpio_config(const gpio_config_t *c) { assert(c->pin_bit_mask == (1ULL << 23) && c->mode == GPIO_MODE_INPUT_OUTPUT); return 0; }
int gpio_hold_en(int gpio) { assert(gpio == 23); s_held = true; return 0; }
int gpio_hold_dis(int gpio) { assert(gpio == 23); s_held = false; return 0; }
void gpio_deep_sleep_hold_en(void) { assert(s_held); }
static void response(const char *text, bool success)
{
    tracker_output_response_begin();
    for (size_t i = 0; i < strlen(text); ++i) tracker_output_response_append(text + i, 1);
    tracker_output_response_finish(success);
}
int main(void)
{
    assert(tracker_output_init(false) && !s_level);
    response("OUTPUT ON", false); assert(!s_level && !tracker_output_take_report_due());
    response("OUTPUT ON", true); assert(s_level && tracker_output_take_report_due());
    assert(!tracker_output_take_report_due());
    response("OUTPUT ON", true); assert(s_level); /* idempotent, never toggle */
    const char *invalid[] = {"OUTPUT TOGGLE", "OUTPUT ON; GPIO26 ON", "OUTPUT OFF\nOUTPUT ON", "output off", "<html>OUTPUT OFF</html>"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) { response(invalid[i], true); assert(s_level); }
    char huge[100]; memset(huge, 'x', sizeof(huge));
    tracker_output_response_begin(); tracker_output_response_append(huge, sizeof(huge)); tracker_output_response_finish(true); assert(s_level);
    char nul[] = "OUTPUT OFF\0";
    tracker_output_response_begin(); tracker_output_response_append(nul, sizeof(nul)); tracker_output_response_finish(true); assert(s_level);
    response("\r\n OUTPUT OFF\t\n", true); assert(!s_level);
    response("OUTPUT ON", true); assert(tracker_output_prepare_sleep() && s_held);
    assert(tracker_output_init(true) && s_level && !s_held); /* sleep retains */
    assert(tracker_output_init(false) && !s_level); /* reset is OFF */
    s_gpio_error = 1; response("OUTPUT ON", true); assert(!s_level);
    tracker_output_status_t status; tracker_output_get_status(&status);
    assert(!status.available && !strcmp(status.result, "gpio_error"));
    puts("Output: fragmented responses, rejection, idempotence, readback, sleep retention and reset OFF passed");
    return 0;
}
