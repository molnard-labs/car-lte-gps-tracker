#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TRACKER_OUTPUT_GPIO 23
#define TRACKER_OUTPUT_RESPONSE_MAX 64u

typedef struct {
    bool enabled;
    bool readback;
    bool available;
    uint32_t applied;
    uint32_t rejected;
    const char *result; /* static: boot, on, off, rejected, gpio_error */
} tracker_output_status_t;

/* Cold/software/watchdog resets start OFF; only deep sleep retains state. */
bool tracker_output_init(bool deep_sleep_wake);
void tracker_output_get_status(tracker_output_status_t *status);
/* HTTP-worker-owned bounded response collection. Only a complete successful
 * authenticated HTTPS response can apply the two exact idempotent commands. */
void tracker_output_response_begin(void);
void tracker_output_response_append(const char *data, size_t length);
void tracker_output_response_finish(bool successful);
/* Main task consumes this flag to report actual pin state promptly. */
bool tracker_output_take_report_due(void);
bool tracker_output_report_pending(void);
/* Called only after network/HTTP work is quiescent; retains pad in deep sleep. */
bool tracker_output_prepare_sleep(void);
void tracker_output_cancel_sleep(void);
