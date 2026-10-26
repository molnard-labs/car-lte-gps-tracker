#pragma once
#include "tracker_types.h"
typedef struct {
    tracker_mode_t mode;
    bool last_ignition, bench;
    int64_t grace_deadline_ms, work_deadline_ms, next_report_ms;
    uint32_t active_interval_s, grace_period_s;
} tracker_policy_t;
void tracker_policy_init(tracker_policy_t *, const tracker_config_t *, int64_t now, bool ignition, bool timer_wake);
void tracker_policy_update(tracker_policy_t *, int64_t now, bool ignition);
bool tracker_policy_report_due(const tracker_policy_t *, int64_t now);
void tracker_policy_reported(tracker_policy_t *, int64_t now);
bool tracker_policy_may_sleep(const tracker_policy_t *, int64_t now, bool ignition, bool setup, bool attempted);
uint32_t tracker_retry_delay_s(unsigned failures);
