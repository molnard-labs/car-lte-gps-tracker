#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Network-order IPv4 or IPv4-mapped IPv6 address; native IPv6 is not setup. */
bool tracker_setup_ap_address(const unsigned char *address, size_t length);
bool tracker_setup_host_allowed(const char *host);
bool tracker_setup_origin_allowed(const char *origin);
bool tracker_setup_host_for_address(const char *host, const char *address);
bool tracker_setup_origin_for_address(const char *origin, const char *address);
bool tracker_setup_credentials_match(const unsigned char *decoded, size_t length, const char *password);
bool tracker_setup_cookie_matches(const char *cookie, const char *session);
bool tracker_setup_flat_json(const char *body);
/* The Wi-Fi-only save route must never change server, SIM or AP credentials. */
bool tracker_setup_wifi_field_allowed(const char *name);

/* Authentication age never controls tracking or the parked sleep policy. */
bool tracker_setup_auth_expired(int64_t now, int64_t started, int64_t last_access);

/* Protected by the setup state lock. A ready apply remains pending until the
 * main task has reloaded storage and successfully reconfigured networking. */
typedef struct {
    bool pending, ready, changed, retry_pin;
    int64_t ready_at_us;
} tracker_setup_apply_t;
bool tracker_setup_apply_request(tracker_setup_apply_t *state);
void tracker_setup_apply_schedule(tracker_setup_apply_t *state, int64_t now,
                                  bool changed, bool retry_pin);
bool tracker_setup_apply_take(tracker_setup_apply_t *state, int64_t now,
                              bool *changed, bool *retry_pin);
void tracker_setup_apply_finish(tracker_setup_apply_t *state);
