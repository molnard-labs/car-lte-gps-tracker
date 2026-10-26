#include "setup_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_lan_host_and_origin(void)
{
    const char *address = "192.168.50.42";
    assert(tracker_setup_host_for_address(address, address));
    assert(tracker_setup_host_for_address("192.168.50.42:80", address));
    const char *bad_hosts[] = {NULL, "", "192.168.4.1", "192.168.50.43", "192.168.50.42.evil.test",
        "evil.test", "192.168.50.42:8080", "192.168.50.42:080", "192.168.50.42:",
        "192.168.50.42/", "192.168.50.42 ", " 192.168.50.42", "192.168.50.42@evil.test",
        "192.168.50.42\r\nHost: evil.test", "[192.168.50.42]"};
    for (size_t i = 0; i < sizeof(bad_hosts) / sizeof(bad_hosts[0]); ++i)
        assert(!tracker_setup_host_for_address(bad_hosts[i], address));
    assert(!tracker_setup_host_for_address(address, NULL));
    assert(!tracker_setup_host_for_address(address, ""));
    assert(!tracker_setup_host_for_address(address, "address-too-long-to-fit-the-port-buffer"));
    assert(tracker_setup_origin_for_address("http://192.168.50.42", address));
    assert(tracker_setup_origin_for_address("http://192.168.50.42:80", address));
    const char *bad_origins[] = {NULL, "", "null", "https://192.168.50.42", "HTTP://192.168.50.42",
        "http://192.168.50.42.evil.test", "http://192.168.50.43", "http://192.168.4.1",
        "http://192.168.50.42:8080", "http://192.168.50.42/", "http://192.168.50.42?query",
        "http://admin@192.168.50.42", "http://192.168.50.42#fragment", "http://192.168.50.42 http://evil.test"};
    for (size_t i = 0; i < sizeof(bad_origins) / sizeof(bad_origins[0]); ++i)
        assert(!tracker_setup_origin_for_address(bad_origins[i], address));
    assert(!tracker_setup_origin_for_address("http://192.168.50.42", NULL));
    assert(!tracker_setup_origin_for_address("http://", ""));
}

static void test_web_credentials(void)
{
    const unsigned char good[] = "admin:local-password";
    const size_t length = sizeof(good) - 1;
    const char *password = "local-password";
    assert(tracker_setup_credentials_match(good, length, password));
    assert(!tracker_setup_credentials_match(NULL, length, password));
    assert(!tracker_setup_credentials_match(good, length, NULL));
    assert(!tracker_setup_credentials_match(good, length, ""));
    assert(!tracker_setup_credentials_match((const unsigned char *)"admin:", 6, ""));
    assert(!tracker_setup_credentials_match(good, 0, password));
    assert(!tracker_setup_credentials_match(good, sizeof(good), password));
    assert(!tracker_setup_credentials_match(good, length, "wrong-password"));
    assert(!tracker_setup_credentials_match((const unsigned char *)"Admin:local-password", length, password));
    assert(!tracker_setup_credentials_match((const unsigned char *)"guest:local-password", length, password));
    assert(!tracker_setup_credentials_match((const unsigned char *)"admin;local-password", length, password));
    /* The decoded payload is a byte sequence: every username/password byte is
     * significant, including embedded NUL. No prefix/truncation may match. */
    for (size_t i = 0; i < length; ++i) {
        unsigned char changed[sizeof(good)]; memcpy(changed, good, sizeof(good));
        changed[i] = 0; assert(!tracker_setup_credentials_match(changed, length, password));
        changed[i] = (unsigned char)(good[i] ^ 1u);
        assert(!tracker_setup_credentials_match(changed, length, password));
        assert(!tracker_setup_credentials_match(good, i, password));
    }
    const unsigned char colon[] = "admin:password:with:colon";
    assert(tracker_setup_credentials_match(colon, sizeof(colon) - 1, "password:with:colon"));
    char maximum[64]; memset(maximum, 'p', sizeof(maximum) - 1); maximum[63] = 0;
    unsigned char maximum_decoded[69]; memcpy(maximum_decoded, "admin:", 6); memcpy(maximum_decoded + 6, maximum, 63);
    assert(tracker_setup_credentials_match(maximum_decoded, sizeof(maximum_decoded), maximum));
}

static void test_read_auth_and_apply_lifecycle(void)
{
    const int64_t minute = 60LL * 1000000;
    tracker_setup_apply_t apply = {0};
    int64_t access = 0;
    /* Continuous status reads outlive the idle deadline, without a tracking
     * barrier. Absolute auth expiry still occurs and never creates a barrier. */
    for (int i = 0; i < 20; ++i) {
        assert(!tracker_setup_auth_expired(i * minute, 0, access));
        access = i * minute;
        assert(!apply.pending);
    }
    assert(tracker_setup_auth_expired(20 * minute, 0, access));
    assert(!apply.pending);
    assert(tracker_setup_auth_expired(5 * minute, 0, 0));
    assert(!tracker_setup_auth_expired(5 * minute - 1, 0, 0));
    bool changed = false, retry = false;
    assert(!tracker_setup_apply_take(&apply, minute, &changed, &retry));
    assert(tracker_setup_apply_request(&apply));
    assert(apply.pending);
    assert(!tracker_setup_apply_request(&apply));
    assert(!tracker_setup_apply_take(&apply, minute, &changed, &retry));
    /* Quiesce/store failure cancels the barrier without a later apply. */
    tracker_setup_apply_finish(&apply);
    assert(!apply.pending);
    assert(!tracker_setup_apply_take(&apply, minute, &changed, &retry));
    assert(tracker_setup_apply_request(&apply));
    tracker_setup_apply_schedule(&apply, minute, true, false);
    assert(!tracker_setup_apply_take(&apply, minute + 299999, &changed, &retry));
    assert(tracker_setup_apply_take(&apply, minute + 300000, &changed, &retry));
    assert(changed && !retry && apply.pending);
    /* Failed network reconfiguration can retry indefinitely without allowing
     * another commit or an old-identity sample between reload and apply. */
    assert(!tracker_setup_apply_take(&apply, 10 * minute, &changed, &retry));
    assert(!tracker_setup_apply_request(&apply));
    assert(apply.pending);
    tracker_setup_apply_finish(&apply);
    assert(tracker_setup_apply_request(&apply));
    tracker_setup_apply_schedule(&apply, minute, false, true);
    assert(tracker_setup_apply_take(&apply, 2 * minute, &changed, &retry));
    assert(!changed && retry && apply.pending);
    tracker_setup_apply_finish(&apply);
    assert(!apply.pending);
}

int main(void)
{
    test_lan_host_and_origin(); test_web_credentials(); test_read_auth_and_apply_lifecycle();
    const char *wifi_fields[] = {"wifi_ssid", "wifi_password", "wifi_secondary_ssid",
        "wifi_secondary_password", "clear_wifi_password", "clear_wifi_secondary_password"};
    for (size_t i = 0; i < sizeof(wifi_fields) / sizeof(wifi_fields[0]); ++i)
        assert(tracker_setup_wifi_field_allowed(wifi_fields[i]));
    const char *other_fields[] = {NULL, "", "setup_password", "clear_setup_password",
        "endpoint", "device_id", "access_client_secret", "sim_pin", "retry_sim_pin", "web_password", "clear_web_password",
        "network_mode", "bench_mode", "clear_wifi_ssid", "wifi_secondary_password_set"};
    for (size_t i = 0; i < sizeof(other_fields) / sizeof(other_fields[0]); ++i)
        assert(!tracker_setup_wifi_field_allowed(other_fields[i]));
    const unsigned char ap4[] = {192, 168, 4, 1};
    const unsigned char sta4[] = {192, 168, 4, 2};
    unsigned char ap6[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255, 192, 168, 4, 1};
    assert(tracker_setup_ap_address(ap4, sizeof(ap4)));
    assert(tracker_setup_ap_address(ap6, sizeof(ap6)));
    assert(!tracker_setup_ap_address(sta4, sizeof(sta4)));
    assert(!tracker_setup_ap_address(ap4, sizeof(ap4) - 1));
    assert(!tracker_setup_ap_address(NULL, sizeof(ap4)));
    ap6[10] = 0; /* IPv4-compatible/native IPv6 must not match the AP. */
    assert(!tracker_setup_ap_address(ap6, sizeof(ap6)));
    ap6[10] = 255;
    ap6[15] = 2;
    assert(!tracker_setup_ap_address(ap6, sizeof(ap6)));
    assert(tracker_setup_host_allowed("192.168.4.1"));
    assert(tracker_setup_host_allowed("192.168.4.1:80"));
    assert(!tracker_setup_host_allowed(NULL));
    assert(!tracker_setup_host_allowed("192.168.4.1.evil.test"));
    assert(!tracker_setup_host_allowed("192.168.4.1:8080"));
    assert(!tracker_setup_host_allowed("evil.test"));
    assert(tracker_setup_origin_allowed("http://192.168.4.1"));
    assert(!tracker_setup_origin_allowed(NULL));
    assert(!tracker_setup_origin_allowed("null"));
    assert(!tracker_setup_origin_allowed("http://192.168.4.1.evil.test"));
    assert(!tracker_setup_origin_allowed("https://192.168.4.1"));
    assert(tracker_setup_cookie_matches("tracker_session=abc", "abc"));
    assert(tracker_setup_cookie_matches("other=foo; tracker_session=abc; x=y", "abc"));
    assert(!tracker_setup_cookie_matches("tracker_session=abcd", "abc"));
    assert(!tracker_setup_cookie_matches("prefix_tracker_session=abc", "abc"));
    assert(!tracker_setup_cookie_matches("tracker_session=abc; tracker_session=abc", "abc"));
    assert(!tracker_setup_cookie_matches("tracker_session=bad; tracker_session=abc", "abc"));
    assert(!tracker_setup_cookie_matches(NULL, "abc"));
    assert(!tracker_setup_cookie_matches("tracker_session=", ""));
    assert(tracker_setup_flat_json("{}"));
    assert(tracker_setup_flat_json(" {\"name\":\"quoted { braces }\",\"number\":30} "));
    assert(tracker_setup_flat_json("{\"secret\":\"quote\\\" and slash\\\\\"}"));
    assert(!tracker_setup_flat_json(NULL));
    assert(!tracker_setup_flat_json("[]"));
    assert(!tracker_setup_flat_json("{\"name\":{\"nested\":true}}"));
    assert(!tracker_setup_flat_json("{\"name\":[1,2]}"));
    assert(!tracker_setup_flat_json("{\"secret\":\"part\\u0000hidden\"}"));
    assert(!tracker_setup_flat_json("{\"secret\":\"unterminated}"));
    assert(!tracker_setup_flat_json("{}{}"));
    assert(!tracker_setup_flat_json("{} trailing"));
    assert(!tracker_setup_flat_json("{\"secret\":\"abc\""));
    puts("setup policy: all checks passed");
    return 0;
}
