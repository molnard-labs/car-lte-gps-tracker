#include "net_policy.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

static void test_wifi_fallback(void)
{
    tracker_wifi_policy_t policy = {0};
    /* A failed primary is disconnected before a three-second radio pause.
     * The secondary then gets its own complete ten-second DHCP budget. */
    assert(tracker_wifi_policy_next(&policy, 1000, true, true, false, false) == 1);
    assert(tracker_wifi_policy_next(&policy, 10999, true, true, false, false) == 0);
    assert(tracker_wifi_policy_next(&policy, 11000, true, true, false, false) == TRACKER_WIFI_DISCONNECT);
    assert(policy.profile == 0);
    assert(tracker_wifi_policy_next(&policy, 13999, true, true, false, false) == 0);
    assert(tracker_wifi_policy_next(&policy, 14000, true, true, false, false) == 2);
    assert(tracker_wifi_policy_next(&policy, 23999, true, true, false, false) == 0);
    assert(tracker_wifi_policy_next(&policy, 24000, true, true, false, false) == TRACKER_WIFI_DISCONNECT);
    assert(tracker_wifi_policy_next(&policy, 38999, true, true, false, false) == 0);
    assert(tracker_wifi_policy_next(&policy, 39000, true, true, false, false) == 1);

    policy = (tracker_wifi_policy_t){0};
    assert(tracker_wifi_policy_next(&policy, 0, false, true, false, false) == 2);
    assert(tracker_wifi_policy_next(&policy, 100, false, true, true, false) == 0);
    assert(tracker_wifi_policy_next(&policy, 999999, false, true, true, false) == 0);
    assert(policy.profile == 2);
    policy = (tracker_wifi_policy_t){0};
    assert(tracker_wifi_policy_next(&policy, 0, false, false, false, true) == 0);
    assert(policy.profile == 0);

    /* Once both fail and LTE works, the next association round waits
     * five minutes; it still gives each saved network a chance. */
    assert(tracker_wifi_policy_next(&policy, 0, true, true, false, true) == 1);
    assert(tracker_wifi_policy_next(&policy, 10000, true, true, false, true) == TRACKER_WIFI_DISCONNECT);
    assert(tracker_wifi_policy_next(&policy, 13000, true, true, false, true) == 2);
    assert(tracker_wifi_policy_next(&policy, 23000, true, true, false, true) == TRACKER_WIFI_DISCONNECT);
    assert(tracker_wifi_policy_next(&policy, 322999, true, true, false, true) == 0);
    assert(tracker_wifi_policy_next(&policy, 323000, true, true, false, true) == 1);
    policy = (tracker_wifi_policy_t){0};
    assert(tracker_wifi_policy_next(&policy, 0, true, false, false, true) == 1);
    assert(tracker_wifi_policy_next(&policy, 10000, true, false, false, true) == TRACKER_WIFI_DISCONNECT);
    assert(tracker_wifi_policy_next(&policy, 20000, true, false, false, false) == 0);
    /* LTE loss must not leave every uplink idle for the old five-minute wait. */
    assert(tracker_wifi_policy_next(&policy, 34999, true, false, false, false) == 0);
    assert(tracker_wifi_policy_next(&policy, 35000, true, false, false, false) == 1);
}

static void test_wifi_stays_connected(void)
{
    tracker_wifi_policy_t policy = {0};
    assert(tracker_wifi_policy_next(&policy, 0, true, true, false, false) == 1);
    /* DHCP success at the timeout boundary wins over a pending retry. */
    assert(tracker_wifi_policy_next(&policy, 10000, true, true, true, false) == 0);
    assert(tracker_wifi_policy_next(&policy, 86400000, true, true, true, false) == 0);
    assert(policy.was_connected && policy.profile == 1);
    policy = (tracker_wifi_policy_t){0};
    assert(tracker_wifi_policy_next(&policy, 0, true, true, false, false) == 1);
    assert(tracker_wifi_policy_next(&policy, 10000, true, true, false, false) == TRACKER_WIFI_DISCONNECT);
    assert(tracker_wifi_policy_next(&policy, 13000, true, true, false, false) == 2);
    assert(tracker_wifi_policy_next(&policy, 23000, true, true, true, false) == 0);
    /* A secondary never probes or roams to primary while it works, even
     * after the former five-minute scan interval or a complete day. */
    assert(tracker_wifi_policy_next(&policy, 323000, true, true, true, false) == 0);
    assert(tracker_wifi_policy_next(&policy, 86400000, true, true, true, false) == 0);
    assert(policy.was_connected && policy.profile == 2);
    /* An actual disconnect yields the radio, then retries primary first. */
    assert(tracker_wifi_policy_next(&policy, 86401000, true, true, false, false) == TRACKER_WIFI_DISCONNECT);
    assert(tracker_wifi_policy_next(&policy, 86403999, true, true, false, false) == 0);
    assert(tracker_wifi_policy_next(&policy, 86404000, true, true, false, false) == 1);
}

static void test_setup_publish_gate(void)
{
    for (unsigned mask = 0; mask < 64; ++mask) {
        bool wifi = (mask & 1) != 0, paused = (mask & 2) != 0;
        bool requested = (mask & 4) != 0, active = (mask & 8) != 0;
        bool ready = (mask & 16) != 0, allowed = (mask & 32) != 0;
        bool actual = tracker_net_setup_publish_allowed(wifi, paused, requested, active, ready, allowed);
        if (paused) assert(!actual);
        else if (!requested && !active) assert(actual);
        else if (!wifi || !requested || !active || !ready || !allowed) assert(!actual);
        else assert(actual);
    }
    assert(tracker_net_setup_publish_allowed(true, false, true, true, true, true));
    assert(!tracker_net_setup_publish_allowed(false, false, true, true, true, true));
}

int main(void)
{
    test_setup_publish_gate();
    test_wifi_fallback();
    test_wifi_stays_connected();
    const int64_t start = 1000000, deadline = start + 15000;
    /* Repeated partial TLS/write/read progress cannot restart the 15s clock. */
    const int64_t progress[] = {start, start + 1000, start + 14999, deadline, deadline + 100};
    const uint32_t remaining[] = {15000, 14000, 1, 0, 0};
    for (unsigned i = 0; i < sizeof(progress) / sizeof(progress[0]); ++i) {
        assert(tracker_net_remaining_ms(progress[i], deadline) == remaining[i]);
        assert(tracker_net_request_expired(progress[i], deadline, false) == (i >= 3));
    }
    assert(tracker_net_request_expired(start, deadline, true));
    assert(tracker_net_remaining_ms(INT64_MIN, INT64_MAX) == UINT32_MAX);

    /* Ignition resumes the device while shutdown is blocked in PPP teardown. */
    uint32_t generation = 41, first = ++generation;
    assert(!tracker_net_sleep_ack_valid(first, generation, first - 1, true));
    ++generation; /* resume cancels first */
    assert(!tracker_net_sleep_ack_valid(first, generation, first, false));
    uint32_t second = ++generation;
    /* The first worker's late completion must not satisfy the second request. */
    assert(!tracker_net_sleep_ack_valid(second, generation, first, true));
    assert(tracker_net_sleep_ack_valid(second, generation, second, true));
    ++generation; /* ignition at the final acknowledgement boundary */
    assert(!tracker_net_sleep_ack_valid(second, generation, second, true));
    puts("tracker network Wi-Fi fallback/deadline/cancellation policy tests passed");
    return 0;
}
