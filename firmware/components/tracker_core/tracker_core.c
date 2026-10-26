#include "tracker_core.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail(char *error, size_t size, const char *message)
{
    if (error && size) snprintf(error, size, "%s", message);
    return false;
}

static bool bounded_string(const char *text, size_t size)
{
    return memchr(text, 0, size) != NULL;
}

static bool printable(const char *text)
{
    for (; *text; ++text) if ((unsigned char)*text < 32 || (unsigned char)*text == 127) return false;
    return true;
}

void tracker_config_defaults(tracker_config_t *config)
{
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->version = TRACKER_CONFIG_VERSION;
    config->network_mode = TRACKER_WIFI_ONLY;
    config->active_interval_s = 30;
    config->grace_period_s = 600;
    config->parked_interval_s = 1800;
    config->bench_mode = true;
    snprintf(config->endpoint, sizeof(config->endpoint), "https://tracker.davidmolnar.dev/ingest");
    snprintf(config->device_id, sizeof(config->device_id), "car");
}

static bool wifi_password_valid(const char *password)
{
    size_t length = strlen(password);
    if (length && (length < 8 || length > 64)) return false;
    if (length == 64)
        for (size_t i = 0; i < length; ++i)
            if (!isxdigit((unsigned char)password[i])) return false;
    return true;
}

bool tracker_config_validate_wifi(const tracker_config_t *config, char *error, size_t size)
{
    if (!config) return fail(error, size, "Missing configuration");
    if (config->version != TRACKER_CONFIG_VERSION) return fail(error, size, "Unsupported configuration version");
#define CHECK_WIFI_STRING(field) \
    if (!bounded_string(config->field, sizeof(config->field)) || !printable(config->field)) \
        return fail(error, size, "Invalid " #field)
    CHECK_WIFI_STRING(wifi_ssid); CHECK_WIFI_STRING(wifi_password);
    CHECK_WIFI_STRING(wifi_secondary_ssid); CHECK_WIFI_STRING(wifi_secondary_password);
#undef CHECK_WIFI_STRING
    if (!wifi_password_valid(config->wifi_password))
        return fail(error, size, "Primary Wi-Fi password must be empty, 8-63 characters, or a 64-digit hexadecimal key");
    if (!wifi_password_valid(config->wifi_secondary_password))
        return fail(error, size, "Secondary Wi-Fi password must be empty, 8-63 characters, or a 64-digit hexadecimal key");
    if (!config->wifi_secondary_ssid[0] && config->wifi_secondary_password[0])
        return fail(error, size, "Secondary Wi-Fi password requires an SSID");
    if (config->wifi_ssid[0] && !strcmp(config->wifi_ssid, config->wifi_secondary_ssid))
        return fail(error, size, "Primary and secondary Wi-Fi SSIDs must differ");
    if (error && size) error[0] = 0;
    return true;
}

bool tracker_config_validate_stored(const tracker_config_t *config, char *error, size_t size)
{
    if (!tracker_config_validate_wifi(config, error, size)) return false;
#define CHECK_STRING(field) \
    if (!bounded_string(config->field, sizeof(config->field)) || !printable(config->field)) \
        return fail(error, size, "Invalid " #field)
    CHECK_STRING(apn);
    CHECK_STRING(apn_user); CHECK_STRING(apn_password); CHECK_STRING(sim_pin);
    CHECK_STRING(endpoint); CHECK_STRING(device_id); CHECK_STRING(access_client_id);
    CHECK_STRING(access_client_secret); CHECK_STRING(setup_password); CHECK_STRING(web_password);
#undef CHECK_STRING
    if (config->network_mode < TRACKER_WIFI_ONLY || config->network_mode > TRACKER_AUTO)
        return fail(error, size, "Invalid network mode");
    const char *at_fields[] = {config->apn, config->apn_user, config->apn_password};
    for (size_t i = 0; i < sizeof(at_fields) / sizeof(at_fields[0]); ++i)
        if (strchr(at_fields[i], '"') || strchr(at_fields[i], '\\')) return fail(error, size, "Unsupported character in cellular credentials");
    size_t pin_length = strlen(config->sim_pin);
    if (pin_length && (pin_length < 4 || pin_length > 8)) return fail(error, size, "SIM PIN must contain 4-8 digits");
    for (size_t i = 0; i < pin_length; ++i)
        if (config->sim_pin[i] < '0' || config->sim_pin[i] > '9') return fail(error, size, "SIM PIN must contain digits only");
    if (config->endpoint[0]) {
        if (strncmp(config->endpoint, "https://", 8) != 0 || !config->endpoint[8])
            return fail(error, size, "Endpoint must use HTTPS");
        const char *host = config->endpoint + 8;
        const char *path = strchr(host, '/');
        if (!path || path == host || strchr(config->endpoint, '?') || strchr(config->endpoint, '#') || strchr(host, '@') || strchr(host, '\\'))
            return fail(error, size, "Endpoint requires a hostname and path, without credentials, query, or fragment");
        const char *colon = memchr(host, ':', (size_t)(path - host));
        const char *host_end = colon ? colon : path;
        if (host_end == host || host_end - host > 253) return fail(error, size, "Invalid endpoint hostname");
        const char *label = host;
        for (const char *p = host; p <= host_end; ++p) {
            if (p == host_end || *p == '.') {
                if (p == label || p - label > 63 || *label == '-' || p[-1] == '-') return fail(error, size, "Invalid endpoint hostname");
                label = p + 1;
            } else if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-'))
                return fail(error, size, "Invalid endpoint hostname");
        }
        if (colon) {
            unsigned port = 0;
            if (colon + 1 == path || path - colon > 6) return fail(error, size, "Invalid endpoint port");
            for (const char *p = colon + 1; p < path; ++p) {
                if (*p < '0' || *p > '9') return fail(error, size, "Invalid endpoint port");
                port = port * 10 + (unsigned)(*p - '0');
            }
            if (!port || port > 65535) return fail(error, size, "Invalid endpoint port");
        }
        for (const char *p = path; *p; ++p)
            if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127) return fail(error, size, "Endpoint path must be URL-encoded ASCII");
    }
    size_t setup_length = strlen(config->setup_password);
    if (setup_length && (setup_length < 12 || setup_length > 63)) return fail(error, size, "Setup passphrase must contain 12-63 characters");
    size_t web_length = strlen(config->web_password);
    if (web_length && (web_length < 8 || web_length > 63))
        return fail(error, size, "Web interface password must contain 8-63 characters, or be empty");
    if (config->active_interval_s < 10 || config->active_interval_s > 300)
        return fail(error, size, "Active interval must be 10-300 seconds");
    if (config->grace_period_s < 60 || config->grace_period_s > 3600)
        return fail(error, size, "Parking grace must be 60-3600 seconds");
    if (config->parked_interval_s < 300 || config->parked_interval_s > 86400)
        return fail(error, size, "Parked interval must be 300-86400 seconds");
    if (error && size) error[0] = 0;
    return true;
}

bool tracker_config_validate(const tracker_config_t *config, char *error, size_t size)
{
    if (!tracker_config_validate_stored(config, error, size)) return false;
    if (!config->endpoint[0]) return fail(error, size, "HTTPS upload URL is required");
    if (!config->device_id[0]) return fail(error, size, "Traccar device ID is required");
    if (config->network_mode != TRACKER_CELLULAR_ONLY && !config->wifi_ssid[0])
        return fail(error, size, "Wi-Fi SSID is required");
    if (config->network_mode != TRACKER_WIFI_ONLY && !config->apn[0])
        return fail(error, size, "Cellular APN is required");
    if (!config->access_client_id[0] || !config->access_client_secret[0])
        return fail(error, size, "Cloudflare Access client ID and client secret are required for uploads");
    if (error && size) error[0] = 0;
    return true;
}

static bool decimal(const char *text, double *value)
{
    if (!text || !*text) return false;
    char *end = NULL;
    double parsed = strtod(text, &end);
    if (end == text || *end || !isfinite(parsed)) return false;
    *value = parsed;
    return true;
}

static bool digits(const char *text, size_t count)
{
    for (size_t i = 0; i < count; ++i) if (text[i] < '0' || text[i] > '9') return false;
    return true;
}

static int two_digits(const char *text) { return (text[0] - '0') * 10 + text[1] - '0'; }

static bool leap(int year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }

static bool parse_time(const char *date, const char *utc, int64_t *timestamp)
{
    size_t utc_length = strlen(utc);
    if (strlen(date) != 6 || !digits(date, 6) || utc_length < 6 || !digits(utc, 6)) return false;
    int day = two_digits(date), month = two_digits(date + 2), year = 2000 + two_digits(date + 4);
    int hour = two_digits(utc), minute = two_digits(utc + 2), second = two_digits(utc + 4);
    static const int days_in_month[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (year < 2020 || month < 1 || month > 12 || day < 1 ||
        day > days_in_month[month - 1] + (month == 2 && leap(year)) || hour > 23 || minute > 59 || second > 59) return false;
    int milliseconds = 0;
    if (utc_length > 6) {
        if (utc[6] != '.' || utc_length == 7 || !digits(utc + 7, utc_length - 7)) return false;
        for (size_t i = 0; i < 3; ++i) milliseconds = milliseconds * 10 + (7 + i < utc_length ? utc[7 + i] - '0' : 0);
    }
    int64_t days = 0;
    for (int y = 1970; y < year; ++y) days += leap(y) ? 366 : 365;
    for (int m = 1; m < month; ++m) days += days_in_month[m - 1] + (m == 2 && leap(year));
    days += day - 1;
    *timestamp = (((days * 24 + hour) * 60 + minute) * 60 + second) * 1000 + milliseconds;
    return true;
}

static bool coordinate(const char *text, const char *hemisphere, bool latitude, double *out)
{
    const char *dot = strchr(text, '.');
    size_t whole = dot ? (size_t)(dot - text) : strlen(text);
    if (whole != (latitude ? 4u : 5u) || !digits(text, whole) || (dot && (!dot[1] || !digits(dot + 1, strlen(dot + 1))))) return false;
    double raw;
    if (!decimal(text, &raw) || strlen(hemisphere) != 1) return false;
    char h = (char)toupper((unsigned char)hemisphere[0]);
    if ((latitude && h != 'N' && h != 'S') || (!latitude && h != 'E' && h != 'W')) return false;
    int degrees = (int)(raw / 100.0);
    double minutes = raw - degrees * 100.0;
    int maximum = latitude ? 90 : 180;
    if (minutes >= 60 || minutes < 0 || degrees > maximum || (degrees == maximum && minutes != 0)) return false;
    *out = (degrees + minutes / 60.0) * ((h == 'S' || h == 'W') ? -1.0 : 1.0);
    return true;
}

bool tracker_fix_is_valid(const tracker_fix_t *fix)
{
    return fix && fix->valid && isfinite(fix->latitude) && isfinite(fix->longitude) &&
        fix->latitude >= -90 && fix->latitude <= 90 && fix->longitude >= -180 && fix->longitude <= 180 &&
        fix->timestamp_ms >= INT64_C(1577836800000) && fix->timestamp_ms < INT64_C(4102444800000) &&
        (!fix->has_altitude || isfinite(fix->altitude_m)) &&
        (!fix->has_speed || (isfinite(fix->speed_kmh) && fix->speed_kmh >= 0)) &&
        (!fix->has_course || (isfinite(fix->course_deg) && fix->course_deg >= 0 && fix->course_deg <= 360));
}

tracker_parse_result_t tracker_parse_cgpsinfo(const char *response, tracker_fix_t *fix)
{
    if (!fix) return TRACKER_PARSE_INVALID;
    memset(fix, 0, sizeof(*fix));
    if (!response) return TRACKER_PARSE_INVALID;
    const char *start = strstr(response, "+CGPSINFO:");
    if (!start) return TRACKER_PARSE_INVALID;
    start += 10;
    while (*start == ' ' || *start == '\t') ++start;
    size_t length = strcspn(start, "\r\n");
    if (!length || length >= 256) return TRACKER_PARSE_INVALID;
    char line[256];
    memcpy(line, start, length); line[length] = 0;
    char *fields[9]; size_t field_count = 1; fields[0] = line;
    for (char *p = line; *p; ++p) if (*p == ',') {
        if (field_count == 9) return TRACKER_PARSE_INVALID;
        *p = 0; fields[field_count++] = p + 1;
    }
    if (field_count != 9) return TRACKER_PARSE_INVALID;
    bool empty = true;
    for (size_t i = 0; i < 9; ++i) {
        char *p = fields[i]; while (*p == ' ' || *p == '\t') ++p; fields[i] = p;
        size_t n = strlen(p); while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) p[--n] = 0;
        if (*p) empty = false;
    }
    if (empty) return TRACKER_PARSE_NO_FIX;
    tracker_fix_t candidate = {0};
    if (!coordinate(fields[0], fields[1], true, &candidate.latitude) ||
        !coordinate(fields[2], fields[3], false, &candidate.longitude) ||
        !parse_time(fields[4], fields[5], &candidate.timestamp_ms)) return TRACKER_PARSE_INVALID;
    candidate.has_altitude = fields[6][0] != 0;
    candidate.has_speed = fields[7][0] != 0;
    candidate.has_course = fields[8][0] != 0;
    if ((candidate.has_altitude && !decimal(fields[6], &candidate.altitude_m)) ||
        (candidate.has_speed && !decimal(fields[7], &candidate.speed_kmh)) ||
        (candidate.has_course && !decimal(fields[8], &candidate.course_deg))) return TRACKER_PARSE_INVALID;
    candidate.speed_kmh *= 1.852;
    candidate.valid = true;
    if (!tracker_fix_is_valid(&candidate)) return TRACKER_PARSE_INVALID;
    *fix = candidate;
    return TRACKER_PARSE_OK;
}

tracker_parse_result_t tracker_parse_cgnssinfo(const char *response, tracker_gnss_status_t *status)
{
    if (!status) return TRACKER_PARSE_INVALID;
    status->details_available = false;
    status->fix_mode = 0;
    status->gps_satellites = status->glonass_satellites = status->beidou_satellites = -1;
    status->hdop = -1;
    if (!response) return TRACKER_PARSE_INVALID;
    const char *start = strstr(response, "+CGNSSINFO:");
    if (!start || (start != response && start[-1] != '\r' && start[-1] != '\n'))
        return TRACKER_PARSE_INVALID;
    start += strlen("+CGNSSINFO:");
    while (*start == ' ' || *start == '\t') ++start;
    size_t length = strcspn(start, "\r\n");
    if (!length || length >= 384) return TRACKER_PARSE_INVALID;
    char line[384];
    memcpy(line, start, length); line[length] = 0;
    char *fields[16]; size_t count = 1; fields[0] = line;
    for (char *p = line; *p; ++p) if (*p == ',') {
        if (count == 16) return TRACKER_PARSE_INVALID;
        *p = 0; fields[count++] = p + 1;
    }
    if (count != 16) return TRACKER_PARSE_INVALID;
    for (size_t i = 0; i < count; ++i) {
        char *p = fields[i]; while (*p == ' ' || *p == '\t') ++p; fields[i] = p;
        size_t n = strlen(p); while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) p[--n] = 0;
    }
    int mode = 0, satellites[3] = {-1, -1, -1};
    double hdop = -1;
    if (fields[0][0]) {
        if (!strcmp(fields[0], "2")) mode = 2;
        else if (!strcmp(fields[0], "3")) mode = 3;
        else return TRACKER_PARSE_INVALID;
    }
    for (size_t i = 0; i < 3; ++i) {
        size_t n = strlen(fields[i + 1]);
        if (!n) continue;
        if (n > 2 || !digits(fields[i + 1], n)) return TRACKER_PARSE_INVALID;
        satellites[i] = atoi(fields[i + 1]);
        if (satellites[i] > 12) return TRACKER_PARSE_INVALID;
    }
    if (fields[14][0] && (!decimal(fields[14], &hdop) || hdop < 0 || hdop > 1000))
        return TRACKER_PARSE_INVALID;
    status->details_available = true;
    status->fix_mode = mode;
    status->gps_satellites = satellites[0];
    status->glonass_satellites = satellites[1];
    status->beidou_satellites = satellites[2];
    status->hdop = hdop;
    return mode ? TRACKER_PARSE_OK : TRACKER_PARSE_NO_FIX;
}

const char *tracker_mode_name(tracker_mode_t mode)
{
    switch (mode) { case TRACKER_ACTIVE: return "active"; case TRACKER_PARKING_GRACE: return "parking_grace";
        case TRACKER_PARKED: return "parked"; default: return "unknown"; }
}
const char *tracker_network_name(tracker_network_t network)
{
    switch (network) { case TRACKER_NET_NONE: return "none"; case TRACKER_NET_WIFI: return "wifi";
        case TRACKER_NET_LTE: return "lte"; default: return "unknown"; }
}
const char *tracker_network_mode_name(tracker_network_mode_t mode)
{
    switch (mode) { case TRACKER_WIFI_ONLY: return "wifi_only"; case TRACKER_CELLULAR_ONLY: return "cellular_only";
        case TRACKER_AUTO: return "auto"; default: return "unknown"; }
}

static bool append(char *buffer, size_t size, size_t *used, const char *format, ...)
{
    if (*used >= size) return false;
    va_list args; va_start(args, format);
    int written = vsnprintf(buffer + *used, size - *used, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= size - *used) return false;
    *used += (size_t)written;
    return true;
}

int tracker_osmand_form(const tracker_config_t *config, const tracker_sample_t *sample, char *buffer, size_t size)
{
    if (!buffer || !size) return -1;
    buffer[0] = 0;
    if (!config || !sample || !bounded_string(config->device_id, sizeof(config->device_id)) || !config->device_id[0] ||
        !tracker_fix_is_valid(&sample->fix) || sample->reported_at_ms < INT64_C(1577836800000) ||
        sample->reported_at_ms >= INT64_C(4102444800000) || sample->mode < TRACKER_ACTIVE || sample->mode > TRACKER_PARKED ||
        sample->network < TRACKER_NET_NONE || sample->network > TRACKER_NET_LTE) return -1;
    size_t used = 0;
    if (!append(buffer, size, &used, "id=")) goto overflow;
    for (const unsigned char *p = (const unsigned char *)config->device_id; *p; ++p) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            if (!append(buffer, size, &used, "%c", *p)) goto overflow;
        } else if (!append(buffer, size, &used, "%%%02X", *p)) goto overflow;
    }
    if (!append(buffer, size, &used, "&timestamp=%" PRId64 "&valid=%s&gpsFix=%s&reported_at=%" PRId64 "&fix_time=%" PRId64
                "&sample_id=%" PRIu64 "&ignition=%s&mode=%s&network=%s",
                sample->gps_fix ? sample->fix.timestamp_ms : sample->reported_at_ms,
                sample->gps_fix ? "true" : "false", sample->gps_fix ? "true" : "false",
                sample->reported_at_ms, sample->fix.timestamp_ms, sample->sequence,
                sample->ignition ? "true" : "false", tracker_mode_name(sample->mode), tracker_network_name(sample->network))) goto overflow;
    if (sample->gps_fix) {
        if (!append(buffer, size, &used, "&lat=%.7f&lon=%.7f", sample->fix.latitude, sample->fix.longitude)) goto overflow;
        if (sample->fix.has_altitude && !append(buffer, size, &used, "&altitude=%.2f", sample->fix.altitude_m)) goto overflow;
        if (sample->fix.has_speed && !append(buffer, size, &used, "&speed=%.3f", sample->fix.speed_kmh)) goto overflow;
        if (sample->fix.has_course && !append(buffer, size, &used, "&bearing=%.2f", sample->fix.course_deg)) goto overflow;
    }
    return (int)used;
overflow:
    buffer[0] = 0;
    return -1;
}
