#include "tracker_output.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#else
#include "test/output_test_platform.h"
#endif

#define OUTPUT_MARKER 0x4f555431u
static RTC_DATA_ATTR uint32_t s_marker;
static RTC_DATA_ATTR bool s_retained;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static tracker_output_status_t s_status;
static bool s_report_due;
static bool s_initialized;
/* This buffer is private to the HTTPS worker, never shared with status APIs. */
static char s_response[TRACKER_OUTPUT_RESPONSE_MAX];
static size_t s_length;
static bool s_overflow;

bool tracker_output_init(bool deep_sleep_wake)
{
    bool enabled = deep_sleep_wake && s_marker == OUTPUT_MARKER && s_retained;
    memset(&s_status, 0, sizeof(s_status));
    s_status.result = "gpio_error";
    s_report_due = false;
    s_initialized = false;
    s_marker = OUTPUT_MARKER;
    s_retained = enabled;
    /* Program latch/direction before releasing a pad retained through sleep. */
    if (gpio_set_level(TRACKER_OUTPUT_GPIO, enabled) != ESP_OK) return false;
    gpio_config_t config = {.pin_bit_mask = 1ULL << TRACKER_OUTPUT_GPIO,
        .mode = GPIO_MODE_INPUT_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE, .intr_type = GPIO_INTR_DISABLE};
    if (gpio_config(&config) != ESP_OK || gpio_hold_dis(TRACKER_OUTPUT_GPIO) != ESP_OK) return false;
    s_status.enabled = enabled;
    s_status.readback = gpio_get_level(TRACKER_OUTPUT_GPIO) != 0;
    s_status.available = s_status.readback == enabled;
    s_status.result = s_status.available ? "boot" : "gpio_error";
    s_initialized = s_status.available;
    return s_status.available;
}

void tracker_output_get_status(tracker_output_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    status->readback = gpio_get_level(TRACKER_OUTPUT_GPIO) != 0;
    status->available = status->available && status->readback == status->enabled;
    portEXIT_CRITICAL(&s_lock);
}

void tracker_output_response_begin(void)
{
    s_length = 0;
    s_overflow = false;
    memset(s_response, 0, sizeof(s_response));
}

void tracker_output_response_append(const char *data, size_t length)
{
    if (!length || s_overflow) return;
    if (!data || length > sizeof(s_response) - s_length) { s_overflow = true; return; }
    memcpy(s_response + s_length, data, length);
    s_length += length;
}

static bool whitespace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

void tracker_output_response_finish(bool successful)
{
    /* Empty responses are normal OsmAnd acknowledgements. Failed, incomplete
     * or cancelled responses never execute a command, even with matching text. */
    if (!successful || (!s_length && !s_overflow)) return;
    size_t start = 0, end = s_length;
    while (start < end && whitespace(s_response[start])) ++start;
    while (end > start && whitespace(s_response[end - 1])) --end;
    if (start == end && !s_overflow) return;
    bool on = !s_overflow && end - start == 9 && !memcmp(s_response + start, "OUTPUT ON", 9);
    bool off = !s_overflow && end - start == 10 && !memcmp(s_response + start, "OUTPUT OFF", 10);
    portENTER_CRITICAL(&s_lock);
    if ((on || off) && s_initialized) {
        bool requested = on;
        if (gpio_set_level(TRACKER_OUTPUT_GPIO, requested) == ESP_OK &&
            (gpio_get_level(TRACKER_OUTPUT_GPIO) != 0) == requested) {
            s_status.enabled = requested;
            s_status.available = true;
            s_retained = requested;
            ++s_status.applied;
            s_status.result = on ? "on" : "off";
        } else {
            s_status.available = false;
            ++s_status.rejected;
            s_status.result = "gpio_error";
        }
    } else {
        ++s_status.rejected;
        s_status.result = s_initialized ? "rejected" : "gpio_error";
    }
    s_report_due = true;
    portEXIT_CRITICAL(&s_lock);
}

bool tracker_output_take_report_due(void)
{
    portENTER_CRITICAL(&s_lock);
    bool due = s_report_due;
    s_report_due = false;
    portEXIT_CRITICAL(&s_lock);
    return due;
}

bool tracker_output_report_pending(void)
{
    portENTER_CRITICAL(&s_lock);
    bool pending = s_report_due;
    portEXIT_CRITICAL(&s_lock);
    return pending;
}

bool tracker_output_prepare_sleep(void)
{
    if (!s_initialized) return true; /* unavailable output must not veto tracking/sleep */
    if (gpio_hold_en(TRACKER_OUTPUT_GPIO) != ESP_OK) return false;
    gpio_deep_sleep_hold_en();
    return true;
}

void tracker_output_cancel_sleep(void) { (void)gpio_hold_dis(TRACKER_OUTPUT_GPIO); }
