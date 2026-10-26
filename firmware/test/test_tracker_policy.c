#include "tracker_policy.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    tracker_config_t c = {.active_interval_s=30, .grace_period_s=600};
    tracker_policy_t p;
    tracker_policy_init(&p,&c,0,true,false);
    assert(p.mode==TRACKER_ACTIVE);
    tracker_policy_update(&p,1000,false);
    assert(p.mode==TRACKER_PARKING_GRACE);
    tracker_policy_update(&p,600999,false);
    assert(p.mode==TRACKER_PARKING_GRACE);
    tracker_policy_update(&p,601000,false);
    assert(p.mode==TRACKER_PARKED);
    assert(!tracker_policy_may_sleep(&p,601000,false,false,false));
    assert(tracker_policy_may_sleep(&p,751000,false,false,false));
    assert(!tracker_policy_may_sleep(&p,751000,true,false,true));
    assert(!tracker_policy_may_sleep(&p,751000,false,true,true));
    tracker_policy_update(&p,751000,true);
    assert(p.mode==TRACKER_ACTIVE && tracker_policy_report_due(&p,751000));
    tracker_policy_reported(&p,751000);
    assert(!tracker_policy_report_due(&p,780999));
    assert(tracker_policy_report_due(&p,781000));
    tracker_policy_init(&p,&c,0,false,true);
    assert(p.mode==TRACKER_PARKED);
    assert(tracker_policy_may_sleep(&p,0,false,false,true));
    c.bench_mode=true;
    tracker_policy_init(&p,&c,0,false,true);
    assert(p.mode==TRACKER_ACTIVE && !tracker_policy_may_sleep(&p,999999,false,false,true));
    assert(tracker_retry_delay_s(0)==0 && tracker_retry_delay_s(1)==5);
    assert(tracker_retry_delay_s(6)==160 && tracker_retry_delay_s(7)==300 && tracker_retry_delay_s(999)==300);
    puts("tracker policy tests passed");
}
