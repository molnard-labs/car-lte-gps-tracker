#pragma once

#include "tracker_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRACKER_PARSE_INVALID = -1,
    TRACKER_PARSE_OK = 0,
    TRACKER_PARSE_NO_FIX = 1
} tracker_parse_result_t;

/** Initialize non-secret commissioning defaults. */
void tracker_config_defaults(tracker_config_t *config);
/** Validate upload readiness and all fields; error never includes secrets. */
bool tracker_config_validate(const tracker_config_t *config, char *error, size_t error_size);
/** Validate only the two Wi-Fi profiles, independently of server readiness. */
bool tracker_config_validate_wifi(const tracker_config_t *config, char *error, size_t error_size);
/** Validate a persisted configuration, permitting incomplete commissioning. */
bool tracker_config_validate_stored(const tracker_config_t *config, char *error, size_t error_size);
/** Parse a complete CGPSINFO response, preserving empty optional fields. */
tracker_parse_result_t tracker_parse_cgpsinfo(const char *response, tracker_fix_t *fix);
/** Parse CGNSSINFO diagnostics only (never used as a position). Empty fields
 * remain unknown. Does not change unrelated runtime diagnostic fields. */
tracker_parse_result_t tracker_parse_cgnssinfo(const char *response, tracker_gnss_status_t *status);
/** Validate a fix before storage or transmission. */
bool tracker_fix_is_valid(const tracker_fix_t *fix);
/** Encode OsmAnd POST parameters. Returns bytes excluding NUL, or -1. */
int tracker_osmand_form(const tracker_config_t *config, const tracker_sample_t *sample,
                        char *buffer, size_t buffer_size);
/** Stable protocol names. Invalid enum values return "unknown". */
const char *tracker_mode_name(tracker_mode_t mode);
const char *tracker_network_name(tracker_network_t network);
const char *tracker_network_mode_name(tracker_network_mode_t mode);

#ifdef __cplusplus
}
#endif
