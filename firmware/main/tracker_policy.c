#include "tracker_policy.h"
#include <string.h>
void tracker_policy_init(tracker_policy_t *p, const tracker_config_t *c, int64_t now, bool ignition, bool timer_wake)
{
    memset(p, 0, sizeof(*p));
    p->bench = c->bench_mode;
    p->active_interval_s = c->active_interval_s;
    p->grace_period_s = c->grace_period_s;
    p->last_ignition = ignition;
    p->mode = (p->bench || ignition) ? TRACKER_ACTIVE : timer_wake ? TRACKER_PARKED : TRACKER_PARKING_GRACE;
    p->grace_deadline_ms = now + (int64_t)c->grace_period_s * 1000;
    p->work_deadline_ms = now + 150000;
    p->next_report_ms = now;
}
void tracker_policy_update(tracker_policy_t *p, int64_t now, bool ignition)
{
    tracker_mode_t before = p->mode;
    if (p->bench || ignition) p->mode = TRACKER_ACTIVE;
    else if (p->last_ignition) {
        p->mode = TRACKER_PARKING_GRACE;
        p->grace_deadline_ms = now + (int64_t)p->grace_period_s * 1000;
    } else if (p->mode == TRACKER_PARKING_GRACE && now >= p->grace_deadline_ms) {
        p->mode = TRACKER_PARKED;
        p->work_deadline_ms = now + 150000;
    }
    if (p->mode != before) p->next_report_ms = now;
    p->last_ignition = ignition;
}
bool tracker_policy_report_due(const tracker_policy_t *p, int64_t now) { return now >= p->next_report_ms; }
void tracker_policy_reported(tracker_policy_t *p, int64_t now)
{ p->next_report_ms = now + (p->mode == TRACKER_PARKED ? 180000 : (int64_t)p->active_interval_s * 1000); }
bool tracker_policy_may_sleep(const tracker_policy_t *p, int64_t now, bool ignition, bool setup, bool attempted)
{ return !p->bench && !ignition && !setup && p->mode == TRACKER_PARKED && (attempted || now >= p->work_deadline_ms); }
uint32_t tracker_retry_delay_s(unsigned failures)
{ return failures == 0 ? 0 : failures > 6 ? 300 : 5u << (failures - 1); }
