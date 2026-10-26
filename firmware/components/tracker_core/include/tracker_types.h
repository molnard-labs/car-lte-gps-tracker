#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TRACKER_CONFIG_VERSION 3u
#define TRACKER_QUEUE_CAPACITY 4096u

typedef enum { TRACKER_WIFI_ONLY, TRACKER_CELLULAR_ONLY, TRACKER_AUTO } tracker_network_mode_t;
typedef enum { TRACKER_ACTIVE, TRACKER_PARKING_GRACE, TRACKER_PARKED } tracker_mode_t;
typedef enum { TRACKER_NET_NONE, TRACKER_NET_WIFI, TRACKER_NET_LTE } tracker_network_t;

typedef struct {
    uint32_t version;
    char wifi_ssid[33];
    char wifi_password[65];
    char apn[101];
    char apn_user[65];
    char apn_password[65];
    char sim_pin[9];
    char endpoint[257];
    char device_id[65];
    char access_client_id[129];
    char access_client_secret[129];
    char setup_password[64];
    tracker_network_mode_t network_mode;
    uint32_t active_interval_s;
    uint32_t grace_period_s;
    uint32_t parked_interval_s;
    bool bench_mode;
    char wifi_secondary_ssid[33];
    char wifi_secondary_password[65];
    char web_password[64];
} tracker_config_t;

typedef struct {
    double latitude;
    double longitude;
    double altitude_m;
    double speed_kmh;
    double course_deg;
    int64_t timestamp_ms;
    bool valid;
    bool has_altitude;
    bool has_speed;
    bool has_course;
} tracker_fix_t;

typedef struct {
    tracker_fix_t fix;
    uint64_t sequence;
    int64_t reported_at_ms;
    tracker_mode_t mode;
    tracker_network_t network;
    bool ignition;
    bool gps_fix;
} tracker_sample_t;

typedef struct {
    bool connected;
    bool connecting;
    bool has_ip;
    uint8_t profile; /* 0: none, 1: primary, 2: secondary */
    char ssid[33];
    char ip[16];
    int rssi;
} tracker_wifi_status_t;

/* Volatile receiver diagnostics. Unknown ages/counts/HDOP use -1; an absent
 * CGNSSINFO satellite field must never be presented as a measured zero.
 * Power flags reflect modem acknowledgements/readback, not antenna continuity. */
typedef struct {
    bool requested;
    bool modem_ready;
    bool receiver_running;
    bool receiver_state_known;
    bool antenna_powered;
    bool antenna_state_known;
    bool details_available;
    char state[16];
    uint32_t poll_count;
    uint32_t error_count;
    int64_t last_reply_age_ms;
    int64_t search_elapsed_ms;
    int64_t detail_age_ms;
    int64_t power_age_ms;
    int fix_mode; /* 0: not reported, 2/3: modem-reported dimension. */
    int gps_satellites;
    int glonass_satellites;
    int beidou_satellites;
    double hdop;
    char error[96];
    char detail_error[96];
} tracker_gnss_status_t;

typedef struct {
    bool configured;
    bool ignition;
    bool gnss_fix;
    bool sim_pin_blocked;
    bool setup_active;
    tracker_mode_t mode;
    tracker_network_t network;
    int64_t last_fix_ms;
    int64_t last_upload_ms;
    uint32_t queue_count;
    uint32_t queue_dropped;
    int last_http_status;
    char sim_status[24];
    char error[96];
} tracker_status_t;
