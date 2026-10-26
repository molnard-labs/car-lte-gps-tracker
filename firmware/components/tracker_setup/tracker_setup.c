#include "tracker_setup.h"
#include "setup_policy.h"
#include "setup_listener.h"
#include "setup_wifi_config.h"
#include "tracker_core.h"
#include "tracker_net.h"
#include "tracker_store.h"
#include "tracker_output.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"

#define SETUP_INITIAL_US (5LL * 60 * 1000000)
#define SETUP_IDLE_US (5LL * 60 * 1000000)
#define SETUP_MAX_CLIENT_US (20LL * 60 * 1000000)
#define SETUP_BODY_LIMIT 3072
#define SETUP_SCAN_LIMIT 16

static const char *TAG = "tracker_setup";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static httpd_handle_t s_server;
static httpd_handle_t s_lan_server;
typedef struct {
    bool station;
    uint32_t address;
    char address_text[16];
    struct ifreq interface;
} web_interface_t;
static web_interface_t s_ap_web, s_lan_web;
static SemaphoreHandle_t s_request_mutex;
static SemaphoreHandle_t s_tracking_mutex;
static tracker_config_t s_config;
static tracker_setup_status_fn s_status_callback;
static void *s_status_context;
static char s_session[33];
static bool s_initialized, s_active;
static tracker_setup_apply_t s_apply;
static bool s_session_closed;
static int64_t s_started_us, s_first_client_us, s_last_activity_us, s_close_at_us;
static int64_t s_auth_started_us, s_auth_access_us, s_lan_retry_us;
static void new_session(void);

extern const unsigned char setup_html_start[] asm("_binary_setup_html_start");
extern const unsigned char setup_html_end[] asm("_binary_setup_html_end");

static void response_headers(httpd_req_t *request)
{
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
}

static esp_err_t json_response(httpd_req_t *request, cJSON *json, const char *status)
{
    char *encoded = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!encoded) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                            "Response allocation failed");
    response_headers(request);
    httpd_resp_set_type(request, "application/json");
    if (status) httpd_resp_set_status(request, status);
    esp_err_t result = httpd_resp_send(request, encoded, HTTPD_RESP_USE_STRLEN);
    free(encoded);
    return result;
}

static esp_err_t error_response(httpd_req_t *request, const char *status, const char *message)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", message);
    return json_response(request, json, status);
}

static esp_err_t ok_response(httpd_req_t *request, const char *message)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", true);
    cJSON_AddStringToObject(json, "message", message);
    return json_response(request, json, NULL);
}

static bool get_header(httpd_req_t *request, const char *name, char *buffer, size_t size)
{
    size_t length = httpd_req_get_hdr_value_len(request, name);
    return length > 0 && length < size &&
           httpd_req_get_hdr_value_str(request, name, buffer, size) == ESP_OK;
}

static bool socket_matches(int socket_fd, const web_interface_t *web)
{
    if (!web || !web->address) return false;
    struct sockaddr_storage local;
    socklen_t length = sizeof(local);
    if (getsockname(socket_fd, (struct sockaddr *)&local, &length) != 0) return false;
    if (local.ss_family == AF_INET) {
        const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)&local;
        return ipv4->sin_addr.s_addr == web->address;
    }
#if CONFIG_LWIP_IPV6
    /* HTTPD creates a dual-stack socket when IPv6 is enabled. IPv4 clients
     * then have an IPv4-mapped IPv6 local address, not AF_INET. */
    if (local.ss_family == AF_INET6) {
        const struct sockaddr_in6 *ipv6 = (const struct sockaddr_in6 *)&local;
        const unsigned char *bytes = (const unsigned char *)&ipv6->sin6_addr;
        static const unsigned char prefix[12] = {0,0,0,0,0,0,0,0,0,0,255,255};
        return !memcmp(bytes, prefix, sizeof(prefix)) && !memcmp(bytes + 12, &web->address, 4);
    }
#endif
    return false;
}

static esp_err_t socket_open(httpd_handle_t server, int socket_fd)
{
    const web_interface_t *web = httpd_get_global_user_ctx(server);
    if (!socket_matches(socket_fd, web) || !web->interface.ifr_name[0]) return ESP_FAIL;
    /* Preserve the listener's interface restriction for accepted traffic. */
    return setsockopt(socket_fd, SOL_SOCKET, SO_BINDTODEVICE,
                      &web->interface, sizeof(web->interface)) == 0 ? ESP_OK : ESP_FAIL;
}

/* Retain the local-address guard on every request in addition to binding the
 * listener and accepted sockets to the AP interface. */
static bool request_allowed(httpd_req_t *request, bool session_required)
{
    char header[256];
    const web_interface_t *web = httpd_get_global_user_ctx(request->handle);
    if (!socket_matches(httpd_req_to_sockfd(request), web)) {
        error_response(request, "403 Forbidden", "Web configuration is available only through Wi-Fi");
        return false;
    }
    if (!get_header(request, "Host", header, sizeof(header)) ||
        !tracker_setup_host_for_address(header, web->address_text)) {
        error_response(request, "403 Forbidden", "Invalid setup host");
        return false;
    }
    bool mutating = request->method == HTTP_POST;
    size_t origin_length = httpd_req_get_hdr_value_len(request, "Origin");
    if ((mutating || origin_length) &&
        (!get_header(request, "Origin", header, sizeof(header)) ||
         !tracker_setup_origin_for_address(header, web->address_text))) {
        error_response(request, "403 Forbidden", "Invalid setup origin");
        return false;
    }
    if (s_config.web_password[0]) {
        unsigned char decoded[96] = {0};
        size_t decoded_length = 0;
        bool authorized = get_header(request, "Authorization", header, sizeof(header)) &&
            !strncmp(header, "Basic ", 6) &&
            mbedtls_base64_decode(decoded, sizeof(decoded), &decoded_length,
                (const unsigned char *)header + 6, strlen(header + 6)) == 0 &&
            tracker_setup_credentials_match(decoded, decoded_length, s_config.web_password);
        memset(decoded, 0, sizeof(decoded));
        memset(header, 0, sizeof(header));
        if (!authorized) {
            httpd_resp_set_hdr(request, "WWW-Authenticate", "Basic realm=\"CarTracker\", charset=\"UTF-8\"");
            error_response(request, "401 Unauthorized", "Web login required (username: admin)");
            return false;
        }
    }
    int64_t now = esp_timer_get_time();
    if (tracker_setup_auth_expired(now, s_auth_started_us, s_auth_access_us)) new_session();
    if (session_required &&
        (!get_header(request, "Cookie", header, sizeof(header)) ||
         !tracker_setup_cookie_matches(header, s_session))) {
        error_response(request, "403 Forbidden", "Open the setup page to start a session");
        return false;
    }
    if (mutating &&
        (!get_header(request, "X-CSRF-Token", header, sizeof(header)) ||
         strcmp(header, s_session))) {
        error_response(request, "403 Forbidden", "Invalid setup session token");
        return false;
    }
    /* Successful reads renew authentication only. They never pause PIN,
     * postpone parked sleep, or extend the bounded AP commissioning window. */
    s_auth_access_us = now;
    return true;
}

static cJSON *read_json(httpd_req_t *request)
{
    char content_type[64];
    if (!get_header(request, "Content-Type", content_type, sizeof(content_type)) ||
        (strcmp(content_type, "application/json") &&
         strcmp(content_type, "application/json; charset=utf-8"))) {
        error_response(request, "415 Unsupported Media Type", "Expected application/json");
        return NULL;
    }
    if (request->content_len == 0 || request->content_len > SETUP_BODY_LIMIT) {
        error_response(request, "413 Content Too Large", "Expected a bounded JSON object");
        return NULL;
    }
    char *body = calloc(1, request->content_len + 1);
    if (!body) { error_response(request, "503 Service Unavailable", "Out of memory"); return NULL; }
    size_t received = 0;
    while (received < request->content_len) {
        int result = httpd_req_recv(request, body + received, request->content_len - received);
        if (result <= 0) {
            free(body);
            error_response(request, "400 Bad Request", "Incomplete request body");
            return NULL;
        }
        received += (size_t)result;
    }
    bool valid = strlen(body) == received && tracker_setup_flat_json(body);
    cJSON *json = valid ? cJSON_ParseWithOpts(body, NULL, true) : NULL;
    memset(body, 0, received);
    free(body);
    if (!cJSON_IsObject(json)) {
        cJSON_Delete(json);
        error_response(request, "400 Bad Request", "Expected a flat JSON object");
        return NULL;
    }
    unsigned count = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, json) {
        if (++count > 32) { valid = false; break; }
        for (const cJSON *other = item->next; other; other = other->next) {
            if (!strcmp(item->string, other->string)) valid = false;
        }
    }
    if (!valid) {
        cJSON_Delete(json);
        error_response(request, "400 Bad Request", "Duplicate or excessive fields");
        return NULL;
    }
    return json;
}

typedef struct { const char *name; size_t offset; size_t size; bool secret; } config_field_t;
#define FIELD(name, secret_field) {#name, offsetof(tracker_config_t, name), sizeof(((tracker_config_t *)0)->name), secret_field}
static const config_field_t s_fields[] = {
    FIELD(wifi_ssid, false), FIELD(wifi_password, true),
    FIELD(wifi_secondary_ssid, false), FIELD(wifi_secondary_password, true), FIELD(apn, false),
    FIELD(apn_user, false), FIELD(apn_password, true), FIELD(sim_pin, true),
    FIELD(endpoint, false), FIELD(device_id, false), FIELD(access_client_id, true),
    FIELD(access_client_secret, true), FIELD(setup_password, true), FIELD(web_password, true),
};

static bool merge_config(const cJSON *json, tracker_config_t *candidate, bool wifi_only,
                         char *error, size_t size)
{
    const cJSON *item;
    const char *invalid_field = NULL;
    cJSON_ArrayForEach(item, json) {
        invalid_field = NULL;
        if (wifi_only && !tracker_setup_wifi_field_allowed(item->string)) goto invalid;
        bool matched = false;
        for (size_t i = 0; i < sizeof(s_fields) / sizeof(s_fields[0]); ++i) {
            const config_field_t *field = &s_fields[i];
            char *destination = (char *)candidate + field->offset;
            if (!strcmp(item->string, field->name)) {
                invalid_field = field->name;
                matched = true;
                if (!cJSON_IsString(item) || strlen(item->valuestring) >= field->size) goto invalid;
                for (const unsigned char *p = (const unsigned char *)item->valuestring; *p; ++p) {
                    if (*p < 32 || *p == 127) goto invalid;
                }
                if (!field->secret || item->valuestring[0]) {
                    memset(destination, 0, field->size);
                    memcpy(destination, item->valuestring, strlen(item->valuestring));
                }
                break;
            }
            if (field->secret && !strncmp(item->string, "clear_", 6) &&
                !strcmp(item->string + 6, field->name)) {
                invalid_field = field->name;
                matched = true;
                if (!cJSON_IsBool(item)) goto invalid;
                if (cJSON_IsTrue(item)) {
                    const cJSON *replacement = cJSON_GetObjectItemCaseSensitive(json, field->name);
                    if (replacement && (!cJSON_IsString(replacement) || replacement->valuestring[0])) goto invalid;
                    memset(destination, 0, field->size);
                }
                break;
            }
        }
        if (matched) continue;
        if (!strcmp(item->string, "network_mode")) {
            invalid_field = "network_mode";
            if (!cJSON_IsString(item)) goto invalid;
            if (!strcmp(item->valuestring, "wifi_only")) candidate->network_mode = TRACKER_WIFI_ONLY;
            else if (!strcmp(item->valuestring, "cellular_only")) candidate->network_mode = TRACKER_CELLULAR_ONLY;
            else if (!strcmp(item->valuestring, "auto")) candidate->network_mode = TRACKER_AUTO;
            else goto invalid;
        } else if (!strcmp(item->string, "bench_mode")) {
            invalid_field = "bench_mode";
            if (!cJSON_IsBool(item)) goto invalid;
            candidate->bench_mode = cJSON_IsTrue(item);
        } else if (!strcmp(item->string, "active_interval_s") ||
                   !strcmp(item->string, "grace_period_s") ||
                   !strcmp(item->string, "parked_interval_s")) {
            invalid_field = !strcmp(item->string, "active_interval_s") ? "active_interval_s" :
                            !strcmp(item->string, "grace_period_s") ? "grace_period_s" : "parked_interval_s";
            double value = item->valuedouble;
            if (!cJSON_IsNumber(item) || !isfinite(value) || value < 0 ||
                value > UINT32_MAX || floor(value) != value) goto invalid;
            if (!strcmp(item->string, "active_interval_s")) candidate->active_interval_s = (uint32_t)value;
            else if (!strcmp(item->string, "grace_period_s")) candidate->grace_period_s = (uint32_t)value;
            else candidate->parked_interval_s = (uint32_t)value;
        } else goto invalid;
    }
    return wifi_only ? tracker_config_validate_wifi(candidate, error, size) :
                       tracker_config_validate_stored(candidate, error, size);
invalid:
    if (invalid_field) snprintf(error, size, "Invalid %s: check value or conflicting clear request", invalid_field);
    else snprintf(error, size, "Invalid or unsupported configuration field");
    return false;
}

static void add_readiness(cJSON *json, const tracker_config_t *config)
{
    cJSON_AddBoolToObject(json, "upload_ready", tracker_config_validate(config, NULL, 0));
    cJSON *missing = cJSON_AddArrayToObject(json, "missing_fields");
    if (!config->endpoint[0]) cJSON_AddItemToArray(missing, cJSON_CreateString("endpoint"));
    if (!config->device_id[0]) cJSON_AddItemToArray(missing, cJSON_CreateString("device_id"));
    if (config->network_mode != TRACKER_CELLULAR_ONLY && !config->wifi_ssid[0])
        cJSON_AddItemToArray(missing, cJSON_CreateString("wifi_ssid"));
    if (config->network_mode != TRACKER_WIFI_ONLY && !config->apn[0])
        cJSON_AddItemToArray(missing, cJSON_CreateString("apn"));
    if (!config->access_client_id[0]) cJSON_AddItemToArray(missing, cJSON_CreateString("access_client_id"));
    if (!config->access_client_secret[0]) cJSON_AddItemToArray(missing, cJSON_CreateString("access_client_secret"));
}

/* Lock order: request -> tracking -> net HTTP -> net PIN. Main calls tick
 * before taking tracking and releases tracking before its next tick. */
static bool begin_apply(httpd_req_t *request, bool *previous_pin_pause)
{
    portENTER_CRITICAL(&s_lock);
    bool accepted = tracker_setup_apply_request(&s_apply);
    portEXIT_CRITICAL(&s_lock);
    if (!accepted) {
        error_response(request, "409 Conflict", "Configuration is still being applied");
        return false;
    }
    /* Announce the barrier before waiting, so an in-progress sleep preparation
     * aborts before deep sleep. No persistent settings have changed yet. */
    if (xSemaphoreTake(s_tracking_mutex, pdMS_TO_TICKS(20000)) != pdTRUE) {
        tracker_setup_apply_complete();
        error_response(request, "503 Service Unavailable", "Tracking operation is busy; retry saving shortly");
        return false;
    }
    esp_err_t result = tracker_net_begin_config_update(previous_pin_pause);
    if (result != ESP_OK) {
        tracker_setup_apply_complete();
        xSemaphoreGive(s_tracking_mutex);
        error_response(request, "503 Service Unavailable", "Network cleanup is busy; settings were not changed");
        return false;
    }
    return true;
}

static void finish_commit(bool committed, bool previous_pin_pause)
{
    tracker_net_end_config_update(committed, previous_pin_pause);
    if (!committed) tracker_setup_apply_complete();
    xSemaphoreGive(s_tracking_mutex);
}

static void schedule_apply(bool changed, bool retry_pin)
{
    portENTER_CRITICAL(&s_lock);
    tracker_setup_apply_schedule(&s_apply, esp_timer_get_time(), changed, retry_pin);
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t page_handler(httpd_req_t *request)
{
    if (!request_allowed(request, false)) return ESP_FAIL;
    char cookie[128];
    snprintf(cookie, sizeof(cookie), "tracker_session=%s; Path=/; HttpOnly; SameSite=Strict", s_session);
    response_headers(request);
    httpd_resp_set_hdr(request, "Set-Cookie", cookie);
    httpd_resp_set_hdr(request, "Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, (const char *)setup_html_start,
                           setup_html_end - setup_html_start - 1);
}

static esp_err_t config_handler(httpd_req_t *request)
{
    if (!request_allowed(request, true)) return ESP_FAIL;
    cJSON *json = cJSON_CreateObject();
    for (size_t i = 0; i < sizeof(s_fields) / sizeof(s_fields[0]); ++i) {
        const config_field_t *field = &s_fields[i];
        const char *value = (const char *)&s_config + field->offset;
        if (field->secret) {
            char key[48];
            snprintf(key, sizeof(key), "%s_set", field->name);
            cJSON_AddBoolToObject(json, key, value[0] != 0);
        } else cJSON_AddStringToObject(json, field->name, value);
    }
    cJSON_AddStringToObject(json, "network_mode", tracker_network_mode_name(s_config.network_mode));
    cJSON_AddNumberToObject(json, "active_interval_s", s_config.active_interval_s);
    cJSON_AddNumberToObject(json, "grace_period_s", s_config.grace_period_s);
    cJSON_AddNumberToObject(json, "parked_interval_s", s_config.parked_interval_s);
    cJSON_AddBoolToObject(json, "bench_mode", s_config.bench_mode);
    cJSON_AddStringToObject(json, "csrf_token", s_session);
    add_readiness(json, &s_config);
    return json_response(request, json, NULL);
}

static esp_err_t save_handler(httpd_req_t *request)
{
    if (!request_allowed(request, true)) return ESP_FAIL;
    cJSON *json = read_json(request);
    if (!json) return ESP_FAIL;
    tracker_config_t candidate = s_config;
    char error[128];
    const cJSON *retry = cJSON_GetObjectItemCaseSensitive(json, "retry_sim_pin");
    if (retry && !cJSON_IsBool(retry)) {
        cJSON_Delete(json);
        memset(&candidate, 0, sizeof(candidate));
        return error_response(request, "422 Unprocessable Content", "Invalid PIN retry request");
    }
    bool retry_pin = cJSON_IsTrue(retry);
    cJSON_DeleteItemFromObjectCaseSensitive(json, "retry_sim_pin");
    bool valid = merge_config(json, &candidate, false, error, sizeof(error));
    cJSON_Delete(json);
    if (!valid) {
        memset(&candidate, 0, sizeof(candidate));
        return error_response(request, "422 Unprocessable Content", error);
    }
    if (retry_pin) {
        bool blocked = false;
        if (tracker_store_get_sim_pin_blocked(&blocked) != ESP_OK || !blocked || !candidate.sim_pin[0]) {
            memset(&candidate, 0, sizeof(candidate));
            return error_response(request, "409 Conflict", "A valid PIN and blocked PIN state are required for retry");
        }
    }
    if (!candidate.setup_password[0]) {
        char factory_password[64] = {0};
        esp_err_t provisioned = tracker_store_get_setup_password(factory_password, sizeof(factory_password));
        memset(factory_password, 0, sizeof(factory_password));
        if (provisioned != ESP_OK) {
            memset(&candidate, 0, sizeof(candidate));
            return error_response(request, "422 Unprocessable Content", "Factory setup password is unavailable; set a new setup password before saving");
        }
    }
    bool previous_pin_pause;
    if (!begin_apply(request, &previous_pin_pause)) {
        memset(&candidate, 0, sizeof(candidate));
        return ESP_FAIL;
    }
    esp_err_t saved = tracker_store_save_config(&candidate);
    if (saved == ESP_OK) s_config = candidate;
    finish_commit(saved == ESP_OK, previous_pin_pause);
    memset(&candidate, 0, sizeof(candidate));
    if (saved == ESP_ERR_INVALID_STATE) return error_response(request, "409 Conflict", "Upload queued positions before changing the device ID or endpoint. If those are unchanged, inspect the storage diagnostic. Existing settings were retained.");
    if (saved != ESP_OK) return error_response(request, "500 Internal Server Error", "Configuration could not be saved; existing settings were retained");
    json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", true);
    cJSON_AddBoolToObject(json, "saved", true);
    cJSON_AddBoolToObject(json, "setup_closing", false);
    cJSON_AddBoolToObject(json, "applying", true);
    add_readiness(json, &s_config);
    esp_err_t result = json_response(request, json, NULL);
    schedule_apply(true, retry_pin);
    return result;
}

static esp_err_t wifi_save_handler(httpd_req_t *request)
{
    if (!request_allowed(request, true)) return ESP_FAIL;
    cJSON *json = read_json(request);
    if (!json) return ESP_FAIL;
    tracker_config_t candidate = s_config;
    char error[128];
    bool valid = merge_config(json, &candidate, true, error, sizeof(error));
    cJSON_Delete(json);
    if (!valid) {
        memset(&candidate, 0, sizeof(candidate));
        return error_response(request, "422 Unprocessable Content", error);
    }
    bool previous_pin_pause;
    if (!begin_apply(request, &previous_pin_pause)) {
        memset(&candidate, 0, sizeof(candidate));
        return ESP_FAIL;
    }
    esp_err_t saved = tracker_store_save_wifi(&candidate);
    finish_commit(saved == ESP_OK, previous_pin_pause);
    if (saved != ESP_OK) {
        memset(&candidate, 0, sizeof(candidate));
        return error_response(request, "500 Internal Server Error", "Wi-Fi settings could not be saved; existing settings were retained");
    }
    tracker_setup_copy_wifi_config(&s_config, &candidate);
    memset(&candidate, 0, sizeof(candidate));
    json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "saved", true);
    cJSON_AddBoolToObject(json, "connection_requested", true);
    cJSON_AddStringToObject(json, "wifi_ssid", s_config.wifi_ssid);
    cJSON_AddStringToObject(json, "wifi_secondary_ssid", s_config.wifi_secondary_ssid);
    cJSON_AddBoolToObject(json, "wifi_password_set", s_config.wifi_password[0] != 0);
    cJSON_AddBoolToObject(json, "wifi_secondary_password_set", s_config.wifi_secondary_password[0] != 0);
    esp_err_t result = json_response(request, json, NULL);
    /* Apply only after the response, including Wi-Fi-only saves. The same
     * barrier protects both transports and persists until main reloads. */
    schedule_apply(true, false);
    return result;
}

static esp_err_t scan_handler(httpd_req_t *request)
{
    if (!request_allowed(request, true)) return ESP_FAIL;
    wifi_ap_record_t *records = calloc(SETUP_SCAN_LIMIT, sizeof(*records));
    if (!records) return error_response(request, "503 Service Unavailable", "Out of memory");
    uint16_t count = SETUP_SCAN_LIMIT;
    esp_err_t result = tracker_net_scan(records, &count);
    if (result != ESP_OK) {
        free(records);
        return error_response(request, "503 Service Unavailable", "Wi-Fi is busy; retry scanning or enter the SSID manually");
    }
    cJSON *json = cJSON_CreateObject();
    cJSON *networks = cJSON_AddArrayToObject(json, "networks");
    for (uint16_t i = 0; i < count && i < SETUP_SCAN_LIMIT; ++i) {
        records[i].ssid[sizeof(records[i].ssid) - 1] = 0;
        if (!records[i].ssid[0]) continue;
        cJSON *network = cJSON_CreateObject();
        cJSON_AddStringToObject(network, "ssid", (char *)records[i].ssid);
        cJSON_AddNumberToObject(network, "rssi", records[i].rssi);
        cJSON_AddBoolToObject(network, "secured", records[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(networks, network);
    }
    free(records);
    return json_response(request, json, NULL);
}

static void diagnostic_number(cJSON *json, const char *name, double value)
{
    if (value < 0) cJSON_AddNullToObject(json, name);
    else cJSON_AddNumberToObject(json, name, value);
}

static esp_err_t status_handler(httpd_req_t *request)
{
    if (!request_allowed(request, true)) return ESP_FAIL;
    tracker_status_t status = {0};
    if (s_status_callback) s_status_callback(&status, s_status_context);
    status.error[sizeof(status.error) - 1] = 0;
    status.sim_status[sizeof(status.sim_status) - 1] = 0;
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "configured", status.configured);
    cJSON_AddBoolToObject(json, "ignition", status.ignition);
    cJSON_AddBoolToObject(json, "gnss_fix", status.gnss_fix);
    cJSON_AddBoolToObject(json, "sim_pin_blocked", status.sim_pin_blocked);
    cJSON_AddStringToObject(json, "sim_status", status.sim_status);
    cJSON_AddBoolToObject(json, "setup_active", tracker_setup_is_active());
    cJSON_AddBoolToObject(json, "setup_ap_active", tracker_net_setup_active());
    cJSON_AddBoolToObject(json, "tracking_paused", tracker_setup_is_applying());
    cJSON_AddBoolToObject(json, "applying", tracker_setup_is_applying());
    cJSON_AddStringToObject(json, "mode", tracker_mode_name(status.mode));
    cJSON_AddStringToObject(json, "network", tracker_network_name(status.network));
    cJSON_AddNumberToObject(json, "last_fix_ms", (double)status.last_fix_ms);
    cJSON_AddNumberToObject(json, "last_upload_ms", (double)status.last_upload_ms);
    cJSON_AddNumberToObject(json, "queue_count", status.queue_count);
    cJSON_AddNumberToObject(json, "queue_dropped", status.queue_dropped);
    cJSON_AddNumberToObject(json, "last_http_status", status.last_http_status);
    cJSON_AddStringToObject(json, "error", status.error);
    tracker_output_status_t output;
    tracker_output_get_status(&output);
    cJSON_AddBoolToObject(json, "output_enabled", output.enabled);
    cJSON_AddBoolToObject(json, "output_readback", output.readback);
    cJSON_AddBoolToObject(json, "output_available", output.available);
    cJSON_AddNumberToObject(json, "output_gpio", TRACKER_OUTPUT_GPIO);
    cJSON_AddNumberToObject(json, "output_applied", output.applied);
    cJSON_AddNumberToObject(json, "output_rejected", output.rejected);
    cJSON_AddStringToObject(json, "output_result", output.result);
    /* Snapshot only: status requests must never issue AT commands or interfere
     * with the single modem owner. Unknown measurements stay JSON null. */
    tracker_gnss_status_t gnss;
    tracker_net_get_gnss_status(&gnss);
    gnss.state[sizeof(gnss.state) - 1] = 0;
    gnss.error[sizeof(gnss.error) - 1] = 0;
    gnss.detail_error[sizeof(gnss.detail_error) - 1] = 0;
    cJSON *gnss_json = cJSON_AddObjectToObject(json, "gnss");
    cJSON_AddStringToObject(gnss_json, "state", gnss.state);
    cJSON_AddBoolToObject(gnss_json, "requested", gnss.requested);
    cJSON_AddBoolToObject(gnss_json, "modem_ready", gnss.modem_ready);
    cJSON_AddBoolToObject(gnss_json, "receiver_running", gnss.receiver_running);
    cJSON_AddBoolToObject(gnss_json, "receiver_state_known", gnss.receiver_state_known);
    cJSON_AddBoolToObject(gnss_json, "antenna_powered", gnss.antenna_powered);
    cJSON_AddBoolToObject(gnss_json, "antenna_state_known", gnss.antenna_state_known);
    cJSON_AddBoolToObject(gnss_json, "details_available", gnss.details_available);
    cJSON_AddNumberToObject(gnss_json, "poll_count", gnss.poll_count);
    cJSON_AddNumberToObject(gnss_json, "error_count", gnss.error_count);
    diagnostic_number(gnss_json, "last_reply_age_ms", (double)gnss.last_reply_age_ms);
    diagnostic_number(gnss_json, "search_elapsed_ms", (double)gnss.search_elapsed_ms);
    diagnostic_number(gnss_json, "detail_age_ms", (double)gnss.detail_age_ms);
    diagnostic_number(gnss_json, "power_age_ms", (double)gnss.power_age_ms);
    diagnostic_number(gnss_json, "fix_mode", gnss.fix_mode ? gnss.fix_mode : -1);
    diagnostic_number(gnss_json, "gps_satellites", gnss.gps_satellites);
    diagnostic_number(gnss_json, "glonass_satellites", gnss.glonass_satellites);
    diagnostic_number(gnss_json, "beidou_satellites", gnss.beidou_satellites);
    diagnostic_number(gnss_json, "hdop", gnss.hdop);
    cJSON_AddStringToObject(gnss_json, "error", gnss.error);
    cJSON_AddStringToObject(gnss_json, "detail_error", gnss.detail_error);
    tracker_wifi_status_t wifi = {0};
    tracker_net_get_wifi_status(&wifi);
    wifi.ssid[sizeof(wifi.ssid) - 1] = 0;
    wifi.ip[sizeof(wifi.ip) - 1] = 0;
    cJSON *wifi_json = cJSON_AddObjectToObject(json, "wifi");
    cJSON_AddBoolToObject(wifi_json, "connected", wifi.connected);
    cJSON_AddBoolToObject(wifi_json, "connecting", wifi.connecting);
    cJSON_AddBoolToObject(wifi_json, "has_ip", wifi.has_ip);
    cJSON_AddNumberToObject(wifi_json, "profile", wifi.profile);
    cJSON_AddStringToObject(wifi_json, "ssid", wifi.ssid);
    cJSON_AddStringToObject(wifi_json, "ip", wifi.ip);
    cJSON_AddNumberToObject(wifi_json, "rssi", wifi.rssi);
    return json_response(request, json, NULL);
}

static esp_err_t action_handler(httpd_req_t *request)
{
    if (!request_allowed(request, true)) return ESP_FAIL;
    cJSON *json = read_json(request);
    if (!json) return ESP_FAIL;
    bool empty = json->child == NULL;
    cJSON_Delete(json);
    if (!empty) return error_response(request, "400 Bad Request", "This action requires an empty JSON object");
    if (!strcmp(request->uri, "/api/activity")) return ok_response(request, "Session active");
    if (!strcmp(request->uri, "/api/close")) {
        /* Closing a LAN view has no networking or authentication side effects. */
        portENTER_CRITICAL(&s_lock);
        if (s_active) s_close_at_us = esp_timer_get_time() + 300000;
        portEXIT_CRITICAL(&s_lock);
        return ok_response(request, "Setup AP window is closing; tracking continues");
    }
    bool pin_retry = !strcmp(request->uri, "/api/pin-retry");
    if (pin_retry) {
        bool blocked = false;
        if (tracker_store_get_sim_pin_blocked(&blocked) != ESP_OK || !blocked || !s_config.sim_pin[0])
            return error_response(request, "409 Conflict", "A saved PIN and blocked PIN state are required");
    }
    bool previous_pin_pause;
    if (!begin_apply(request, &previous_pin_pause)) return ESP_FAIL;
    esp_err_t restored = pin_retry ? ESP_OK : tracker_store_rollback_config(&s_config);
    finish_commit(restored == ESP_OK, previous_pin_pause);
    if (restored == ESP_ERR_INVALID_STATE)
        return error_response(request, "409 Conflict", "Upload queued positions before restoring a different device ID or endpoint. If those are unchanged, inspect the storage diagnostic.");
    if (restored != ESP_OK)
        return error_response(request, "409 Conflict", "No valid previous configuration could be restored");
    esp_err_t result = ok_response(request, "Applying configuration; tracking resumes automatically");
    schedule_apply(!pin_retry, pin_retry);
    return result;
}

static const httpd_uri_t s_routes[] = {
    {.uri = "/", .method = HTTP_GET, .handler = page_handler},
    {.uri = "/api/config", .method = HTTP_GET, .handler = config_handler},
    {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
    {.uri = "/api/scan", .method = HTTP_GET, .handler = scan_handler},
    {.uri = "/api/config", .method = HTTP_POST, .handler = save_handler},
    {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_save_handler},
    {.uri = "/api/close", .method = HTTP_POST, .handler = action_handler},
    {.uri = "/api/restore", .method = HTTP_POST, .handler = action_handler},
    {.uri = "/api/pin-retry", .method = HTTP_POST, .handler = action_handler},
    {.uri = "/api/activity", .method = HTTP_POST, .handler = action_handler},
};

/* AP and LAN HTTPD tasks share configuration/session state. Serialize complete
 * requests, including store commits and secret changes, across both servers. */
static esp_err_t dispatch_request(httpd_req_t *request)
{
    if (xSemaphoreTake(s_request_mutex, pdMS_TO_TICKS(6000)) != pdTRUE)
        return error_response(request, "503 Service Unavailable", "Another configuration request is busy; retry");
    const httpd_uri_t *route = request->user_ctx;
    esp_err_t result = route->handler(request);
    xSemaphoreGive(s_request_mutex);
    return result;
}

static void keep_static_context(void *context) { (void)context; }

static void new_session(void)
{
    s_auth_started_us = s_auth_access_us = esp_timer_get_time();
    uint8_t random[16];
    esp_fill_random(random, sizeof(random));
    for (unsigned i = 0; i < sizeof(random); ++i)
        snprintf(s_session + i * 2, 3, "%02x", random[i]);
    memset(random, 0, sizeof(random));
}

static esp_err_t start_web(httpd_handle_t *server, web_interface_t *web, bool station,
                           uint32_t address)
{
    const char *key = station ? "WIFI_STA_DEF" : "WIFI_AP_DEF";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(key);
    if (!netif) return ESP_ERR_INVALID_STATE;
    memset(web, 0, sizeof(*web));
    web->station = station;
    web->address = address;
    struct in_addr ip = {.s_addr = address};
    if (!inet_ntop(AF_INET, &ip, web->address_text, sizeof(web->address_text)) ||
        esp_netif_get_netif_impl_name(netif, web->interface.ifr_name) != ESP_OK ||
        !web->interface.ifr_name[0]) return ESP_ERR_INVALID_STATE;
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.stack_size = 8192;
    http.max_uri_handlers = sizeof(s_routes) / sizeof(s_routes[0]);
    http.max_open_sockets = 3;
    http.ctrl_port = station ? 32769 : 32768;
    http.lru_purge_enable = true;
    http.recv_wait_timeout = 5;
    http.send_wait_timeout = 5;
    http.open_fn = socket_open;
    http.global_user_ctx = web;
    http.global_user_ctx_free_fn = keep_static_context;
    esp_err_t result = tracker_httpd_start_wifi(server, &http, key, address);
    if (result != ESP_OK) return result;
    for (size_t i = 0; i < sizeof(s_routes) / sizeof(s_routes[0]); ++i) {
        httpd_uri_t route = s_routes[i];
        route.handler = dispatch_request;
        route.user_ctx = (void *)&s_routes[i];
        result = httpd_register_uri_handler(*server, &route);
        if (result != ESP_OK) {
            httpd_stop(*server);
            *server = NULL;
            return result;
        }
    }
    ESP_LOGI(TAG, "%s web configuration ready at http://%s", station ? "Wi-Fi LAN" : "Setup AP", web->address_text);
    return ESP_OK;
}

static void maintain_lan_web(int64_t now)
{
    tracker_wifi_status_t wifi;
    tracker_net_get_wifi_status(&wifi);
    struct in_addr ip = {0};
    bool online = wifi.connected && wifi.has_ip && inet_pton(AF_INET, wifi.ip, &ip) == 1;
    if (s_lan_server && (!online || s_lan_web.address != ip.s_addr)) {
        httpd_stop(s_lan_server);
        s_lan_server = NULL;
        memset(&s_lan_web, 0, sizeof(s_lan_web));
    }
    if (online && !s_lan_server && now >= s_lan_retry_us) {
        s_lan_retry_us = now + 5000000;
        esp_err_t result = start_web(&s_lan_server, &s_lan_web, true, ip.s_addr);
        if (result != ESP_OK) ESP_LOGW(TAG, "LAN web startup failed: %s; will retry", esp_err_to_name(result));
    }
}

esp_err_t tracker_setup_init(tracker_setup_status_fn callback, void *context)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    s_request_mutex = xSemaphoreCreateMutex();
    s_tracking_mutex = xSemaphoreCreateMutex();
    if (!s_request_mutex || !s_tracking_mutex) {
        if (s_request_mutex) vSemaphoreDelete(s_request_mutex);
        if (s_tracking_mutex) vSemaphoreDelete(s_tracking_mutex);
        s_request_mutex = s_tracking_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    bool configured;
    esp_err_t result = tracker_store_load_config(&s_config, &configured);
    if (result != ESP_OK) {
        vSemaphoreDelete(s_request_mutex); vSemaphoreDelete(s_tracking_mutex);
        s_request_mutex = s_tracking_mutex = NULL; return result;
    }
    new_session();
    s_status_callback = callback;
    s_status_context = context;
    s_initialized = true;
    return ESP_OK;
}

esp_err_t tracker_setup_start(const tracker_config_t *config)
{
    if (!s_initialized || !config || s_active) return ESP_ERR_INVALID_STATE;
    char password[64] = {0};
    if (config->setup_password[0]) {
        size_t length = strnlen(config->setup_password, sizeof(config->setup_password));
        if (length < 12 || length >= sizeof(password)) return ESP_ERR_INVALID_ARG;
        memcpy(password, config->setup_password, length);
    } else if (tracker_store_get_setup_password(password, sizeof(password)) != ESP_OK) {
        ESP_LOGE(TAG, "Setup unavailable: unique passphrase has not been provisioned");
        return ESP_ERR_NOT_FOUND;
    }
    if (strnlen(password, sizeof(password)) < 12 || strnlen(password, sizeof(password)) >= sizeof(password)) {
        memset(password, 0, sizeof(password));
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) { memset(password, 0, sizeof(password)); return ESP_FAIL; }
    char ssid[33];
    snprintf(ssid, sizeof(ssid), "CarTracker-%02X%02X%02X", mac[3], mac[4], mac[5]);
    esp_err_t result = tracker_net_setup_start(ssid, password);
    memset(password, 0, sizeof(password));
    if (result != ESP_OK) return result;
    s_config = *config;
    /* Resolve the AP interface only once its netif is up. */
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    int64_t deadline = esp_timer_get_time() + 3000000;
    while (ap && !esp_netif_is_netif_up(ap) && esp_timer_get_time() < deadline)
        vTaskDelay(pdMS_TO_TICKS(20));
    result = start_web(&s_server, &s_ap_web, false, htonl(0xc0a80401u));
    if (result != ESP_OK) { tracker_net_setup_stop(); return result; }
    result = tracker_net_setup_allow_wifi();
    if (result != ESP_OK) {
        httpd_stop(s_server); s_server = NULL;
        tracker_net_setup_stop();
        return result;
    }
    portENTER_CRITICAL(&s_lock);
    s_started_us = s_last_activity_us = esp_timer_get_time();
    s_first_client_us = s_close_at_us = 0;
    tracker_setup_apply_finish(&s_apply);
    s_active = true;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Setup network %s is open for a bounded commissioning window", ssid);
    return ESP_OK;
}

void tracker_setup_tick(void)
{
    if (!s_initialized) return;
    int64_t now = esp_timer_get_time();
    maintain_lan_web(now);
    wifi_sta_list_t stations = {0};
    bool associated = s_active && esp_wifi_ap_get_sta_list(&stations) == ESP_OK && stations.num > 0;
    xSemaphoreTake(s_request_mutex, portMAX_DELAY);
    portENTER_CRITICAL(&s_lock);
    if (associated && !s_first_client_us) s_first_client_us = s_last_activity_us = now;
    bool explicit_close = s_close_at_us && now >= s_close_at_us;
    bool ap_expired = s_active && (!s_first_client_us ? now - s_started_us >= SETUP_INITIAL_US :
        now - s_first_client_us >= SETUP_MAX_CLIENT_US || now - s_last_activity_us >= SETUP_IDLE_US);
    bool close_ap = (explicit_close || ap_expired) && !s_apply.pending;
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_request_mutex);
    /* Never stop HTTPD holding either mutex: its task may be completing a save. */
    if (close_ap && s_active) {
        if (s_server) { httpd_stop(s_server); s_server = NULL; }
        /* A LAN save can start after the close decision. Recheck under the
         * tracking guard, and drain a timed-out HTTP worker before AP->STA can
         * disable Wi-Fi in cellular-only mode. Never hold this over HTTPD stop. */
        tracker_setup_tracking_lock();
        if (tracker_setup_is_applying()) {
            tracker_setup_tracking_unlock();
            return;
        }
        bool previous_pin_pause;
        esp_err_t result = tracker_net_begin_config_update(&previous_pin_pause);
        if (result == ESP_OK) {
            result = tracker_net_setup_stop();
            tracker_net_end_config_update(false, previous_pin_pause);
        }
        tracker_setup_tracking_unlock();
        if (result != ESP_OK || tracker_net_setup_active()) return;
        portENTER_CRITICAL(&s_lock);
        s_active = false;
        s_close_at_us = 0;
        s_session_closed = true;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "Setup AP window closed; Wi-Fi LAN web configuration remains available");
    }
}

bool tracker_setup_is_applying(void)
{
    portENTER_CRITICAL(&s_lock);
    bool pending = s_apply.pending;
    portEXIT_CRITICAL(&s_lock);
    return pending;
}

bool tracker_setup_is_active(void)
{
    portENTER_CRITICAL(&s_lock);
    bool active = s_active || s_apply.pending;
    portEXIT_CRITICAL(&s_lock);
    return active;
}

void tracker_setup_tracking_lock(void)
{
    xSemaphoreTake(s_tracking_mutex, portMAX_DELAY);
}

void tracker_setup_tracking_unlock(void)
{
    xSemaphoreGive(s_tracking_mutex);
}

bool tracker_setup_take_apply(bool *changed, bool *retry_pin)
{
    portENTER_CRITICAL(&s_lock);
    bool ready = tracker_setup_apply_take(&s_apply, esp_timer_get_time(), changed, retry_pin);
    portEXIT_CRITICAL(&s_lock);
    return ready;
}

void tracker_setup_apply_complete(void)
{
    portENTER_CRITICAL(&s_lock);
    tracker_setup_apply_finish(&s_apply);
    portEXIT_CRITICAL(&s_lock);
}

bool tracker_setup_take_session_closed(void)
{
    portENTER_CRITICAL(&s_lock);
    bool closed = s_session_closed;
    s_session_closed = false;
    portEXIT_CRITICAL(&s_lock);
    return closed;
}
