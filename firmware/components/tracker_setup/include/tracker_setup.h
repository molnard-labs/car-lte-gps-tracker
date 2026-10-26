#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "tracker_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*tracker_setup_status_fn)(tracker_status_t *status, void *context);

/** Initialize the setup controller. The callback must be safe from the HTTP task. */
esp_err_t tracker_setup_init(tracker_setup_status_fn status_callback, void *context);
/** Open the bounded setup window. Caller permits this only on power-on boot. */
esp_err_t tracker_setup_start(const tracker_config_t *config);
/** Maintain the LAN listener and bounded AP window. Never call while tracking is locked. */
void tracker_setup_tick(void);
/** Only AP commissioning or actual application inhibits parked sleep. */
bool tracker_setup_is_active(void);
/** Actual mutation barrier; reading or typing never sets this state. */
bool tracker_setup_is_applying(void);
/** Main task: hold across reload, sampling, publish/ACK and sleep preparation.
 * Request handlers use the same mutex before committing any new settings. */
void tracker_setup_tracking_lock(void);
void tracker_setup_tracking_unlock(void);
/** Called under the tracking lock; the barrier remains until complete. */
bool tracker_setup_take_apply(bool *changed, bool *retry_pin);
void tracker_setup_apply_complete(void);
/** Only AP teardown needs to resume cellular/PIN eligibility. */
bool tracker_setup_take_session_closed(void);

#ifdef __cplusplus
}
#endif
