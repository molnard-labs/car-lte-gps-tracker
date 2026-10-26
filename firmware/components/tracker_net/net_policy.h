#pragma once
#include <stdbool.h>
#include <stdint.h>

#define TRACKER_WIFI_ATTEMPT_MS 10000
#define TRACKER_WIFI_OFFLINE_RETRY_MS 15000
#define TRACKER_WIFI_LTE_RETRY_MS 300000
#define TRACKER_WIFI_PROFILE_GAP_MS 3000
#define TRACKER_WIFI_DISCONNECT 3u

/* Owned by the network worker. Each round tries configured profiles in order;
 * either working profile is kept until the connection actually disappears.
 * Return 1/2 to connect, DISCONNECT to end an attempt, or zero to leave it alone. */
typedef struct {
    uint8_t attempted;
    uint8_t profile;
    bool was_connected;
    bool cellular_backoff;
    int64_t attempt_deadline_ms;
    int64_t next_round_ms;
} tracker_wifi_policy_t;

static inline uint8_t tracker_wifi_policy_next(tracker_wifi_policy_t *policy,
                                              int64_t now, bool primary,
                                              bool secondary, bool connected,
                                              bool cellular_connected)
{
    if (connected) {
        if (!policy->was_connected) {
            policy->was_connected = true;
            policy->attempt_deadline_ms = 0;
            policy->attempted = 0;
            policy->next_round_ms = 0;
            policy->cellular_backoff = false;
        }
        return 0;
    }
    if (policy->was_connected) {
        policy->was_connected = false;
        policy->attempted = 0;
        policy->profile = 0;
        policy->attempt_deadline_ms = 0;
        policy->next_round_ms = now + TRACKER_WIFI_PROFILE_GAP_MS;
        return TRACKER_WIFI_DISCONNECT;
    }
    if (policy->cellular_backoff && !cellular_connected) {
        int64_t offline_retry = now + TRACKER_WIFI_OFFLINE_RETRY_MS;
        if (policy->next_round_ms > offline_retry) policy->next_round_ms = offline_retry;
        policy->cellular_backoff = false;
    }
    if (now < policy->attempt_deadline_ms || now < policy->next_round_ms) return 0;
    uint8_t configured = (primary ? 1u : 0u) | (secondary ? 2u : 0u);
    if (!configured) return 0;
    if (policy->attempt_deadline_ms) {
        /* A failed attempt first disconnects, then yields the radio before
         * changing profiles. A late DHCP event can still win before this call. */
        policy->profile = 0;
        policy->attempt_deadline_ms = 0;
        if (configured & (uint8_t)~policy->attempted) {
            policy->next_round_ms = now + TRACKER_WIFI_PROFILE_GAP_MS;
        } else {
            policy->attempted = 0;
            policy->cellular_backoff = cellular_connected;
            policy->next_round_ms = now + (cellular_connected ?
                TRACKER_WIFI_LTE_RETRY_MS : TRACKER_WIFI_OFFLINE_RETRY_MS);
        }
        return TRACKER_WIFI_DISCONNECT;
    }
    uint8_t remaining = configured & (uint8_t)~policy->attempted;
    uint8_t profile = (remaining & 1u) ? 1u : 2u;
    policy->attempted |= profile;
    policy->profile = profile;
    policy->attempt_deadline_ms = now + TRACKER_WIFI_ATTEMPT_MS;
    return profile;
}

/* One absolute deadline is shared by caller, transport worker and cleanup.
 * Progress, EAGAIN and retries must never replenish this request budget. */
static inline uint32_t tracker_net_remaining_ms(int64_t now, int64_t deadline)
{
    if (now >= deadline) return 0;
    uint64_t remaining = (uint64_t)deadline - (uint64_t)now;
    return remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
}

static inline bool tracker_net_request_expired(int64_t now, int64_t deadline, bool canceled)
{
    return canceled || tracker_net_remaining_ms(now, deadline) == 0;
}

/* Resume invalidates an outstanding request even if its worker later finishes.
 * An acknowledgement from a previous request can never authorize deep sleep. */
static inline bool tracker_net_sleep_ack_valid(uint32_t request, uint32_t current,
                                               uint32_t acknowledged, bool requested)
{
    return requested && request == current && request == acknowledged;
}

/* AP setup may publish over an already permitted STA connection. Initial
 * uplink quiescence, sleep preparation, and cellular during setup stay blocked. */
static inline bool tracker_net_setup_publish_allowed(bool wifi, bool paused,
        bool setup_requested, bool ap_active, bool setup_ready, bool wifi_allowed)
{
    if (paused) return false;
    if (!setup_requested && !ap_active) return true;
    return wifi && setup_requested && ap_active && setup_ready && wifi_allowed;
}
