#pragma once
#include "tracker_types.h"
#include <string.h>

/* The deferred apply object contains only Wi-Fi credentials and its schema.
 * Keep the version valid when copying into a zeroed handoff buffer. */
static inline void tracker_setup_copy_wifi_config(tracker_config_t *destination,
                                                  const tracker_config_t *source)
{
    destination->version = TRACKER_CONFIG_VERSION;
    memcpy(destination->wifi_ssid, source->wifi_ssid, sizeof(destination->wifi_ssid));
    memcpy(destination->wifi_password, source->wifi_password, sizeof(destination->wifi_password));
    memcpy(destination->wifi_secondary_ssid, source->wifi_secondary_ssid,
           sizeof(destination->wifi_secondary_ssid));
    memcpy(destination->wifi_secondary_password, source->wifi_secondary_password,
           sizeof(destination->wifi_secondary_password));
}
