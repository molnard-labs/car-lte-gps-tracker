#include "setup_policy.h"
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

bool tracker_setup_wifi_field_allowed(const char *name)
{
    static const char *const names[] = {
        "wifi_ssid", "wifi_password", "wifi_secondary_ssid", "wifi_secondary_password",
        "clear_wifi_password", "clear_wifi_secondary_password",
    };
    if (!name) return false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (!strcmp(name, names[i])) return true;
    }
    return false;
}

bool tracker_setup_ap_address(const unsigned char *address, size_t length)
{
    static const unsigned char ipv4[] = {192, 168, 4, 1};
    static const unsigned char mapped[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255,
                                           192, 168, 4, 1};
    return address && ((length == sizeof(ipv4) && !memcmp(address, ipv4, length)) ||
                       (length == sizeof(mapped) && !memcmp(address, mapped, length)));
}

bool tracker_setup_host_allowed(const char *host)
{
    return tracker_setup_host_for_address(host, "192.168.4.1");
}

bool tracker_setup_origin_allowed(const char *origin)
{
    return tracker_setup_origin_for_address(origin, "192.168.4.1");
}

bool tracker_setup_host_for_address(const char *host, const char *address)
{
    if (!host || !address || !address[0]) return false;
    char with_port[32];
    int size = snprintf(with_port, sizeof(with_port), "%s:80", address);
    return size > 0 && (size_t)size < sizeof(with_port) &&
           (!strcmp(host, address) || !strcmp(host, with_port));
}

bool tracker_setup_origin_for_address(const char *origin, const char *address)
{
    return origin && !strncmp(origin, "http://", 7) &&
           tracker_setup_host_for_address(origin + 7, address);
}

bool tracker_setup_credentials_match(const unsigned char *decoded, size_t length, const char *password)
{
    if (!decoded || !password || !password[0]) return false;
    size_t password_length = strlen(password);
    if (length != 6 + password_length) return false;
    unsigned difference = 0;
    for (size_t i = 0; i < 6; ++i) difference |= decoded[i] ^ (unsigned char)"admin:"[i];
    for (size_t i = 0; i < password_length; ++i) difference |= decoded[6 + i] ^ (unsigned char)password[i];
    return difference == 0;
}

bool tracker_setup_cookie_matches(const char *cookie, const char *session)
{
    static const char key[] = "tracker_session=";
    if (!cookie || !session || !session[0]) return false;
    size_t session_length = strlen(session);
    unsigned matches = 0;
    while (*cookie) {
        while (*cookie == ' ' || *cookie == ';') ++cookie;
        const char *end = strchr(cookie, ';');
        size_t length = end ? (size_t)(end - cookie) : strlen(cookie);
        if (length >= sizeof(key) - 1 && !strncmp(cookie, key, sizeof(key) - 1)) {
            if (length != sizeof(key) - 1 + session_length ||
                memcmp(cookie + sizeof(key) - 1, session, session_length)) return false;
            ++matches;
        }
        if (!end) break;
        cookie = end + 1;
    }
    return matches == 1;
}

/* Structural preflight only; a JSON parser must still check syntax. */
bool tracker_setup_flat_json(const char *body)
{
    if (!body) return false;
    bool quoted = false, escaped = false, opened = false, closed = false;
    unsigned depth = 0;
    for (const unsigned char *p = (const unsigned char *)body; *p; ++p) {
        if (quoted) {
            if (escaped) {
                if (*p == 'u' && !strncmp((const char *)p, "u0000", 5)) return false;
                escaped = false;
            } else if (*p == '\\') escaped = true;
            else if (*p == '"') quoted = false;
            continue;
        }
        if (closed && !isspace(*p)) return false;
        if (*p == '"') quoted = true;
        else if (*p == '[' || *p == ']') return false;
        else if (*p == '{') { if (opened || ++depth != 1) return false; opened = true; }
        else if (*p == '}') { if (depth != 1) return false; depth = 0; closed = true; }
        else if (!opened && !isspace(*p)) return false;
    }
    return opened && closed && !quoted && !escaped;
}

bool tracker_setup_auth_expired(int64_t now, int64_t started, int64_t last_access)
{
    return now - last_access >= 5LL * 60 * 1000000 ||
           now - started >= 20LL * 60 * 1000000;
}

bool tracker_setup_apply_request(tracker_setup_apply_t *state)
{
    if (state->pending) return false;
    *state = (tracker_setup_apply_t){.pending = true};
    return true;
}

void tracker_setup_apply_schedule(tracker_setup_apply_t *state, int64_t now,
                                  bool changed, bool retry_pin)
{
    state->changed = changed;
    state->retry_pin = retry_pin;
    state->ready_at_us = now + 300000;
    state->ready = true;
}

bool tracker_setup_apply_take(tracker_setup_apply_t *state, int64_t now,
                              bool *changed, bool *retry_pin)
{
    if (!state->pending || !state->ready || now < state->ready_at_us) return false;
    *changed = state->changed;
    *retry_pin = state->retry_pin;
    state->ready = false;
    return true;
}

void tracker_setup_apply_finish(tracker_setup_apply_t *state)
{
    *state = (tracker_setup_apply_t){0};
}
