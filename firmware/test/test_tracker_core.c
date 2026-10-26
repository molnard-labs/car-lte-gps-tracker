#include "tracker_core.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static tracker_config_t valid_config(void)
{
    tracker_config_t config;
    tracker_config_defaults(&config);
    strcpy(config.wifi_ssid, "test-network"); strcpy(config.wifi_password, "password123");
    strcpy(config.access_client_id, "test-client"); strcpy(config.access_client_secret, "test-secret");
    return config;
}

static void test_config(void)
{
    tracker_config_t config = valid_config(), changed;
    char error[120];
    assert(tracker_config_validate(&config, error, sizeof(error)) && !error[0]);
    assert(config.active_interval_s == 30 && config.grace_period_s == 600 && config.parked_interval_s == 1800);
    changed = config; memset(changed.wifi_ssid, 'a', sizeof(changed.wifi_ssid));
    assert(!tracker_config_validate(&changed, error, sizeof(error)));
    changed = config; strcpy(changed.sim_pin, "12a4"); assert(!tracker_config_validate(&changed, NULL, 0));
    changed = config; strcpy(changed.sim_pin, "01234567"); assert(tracker_config_validate(&changed, NULL, 0));
    changed = config; changed.network_mode = TRACKER_CELLULAR_ONLY; changed.wifi_ssid[0] = 0;
    assert(!tracker_config_validate(&changed, NULL, 0)); strcpy(changed.apn, "internet");
    assert(tracker_config_validate(&changed, NULL, 0)); strcpy(changed.apn_user, "x\";AT");
    assert(!tracker_config_validate(&changed, NULL, 0));
    const char *bad_urls[] = {"http://example.com/", "https:///ingest", "https://user@example.com/", "https://example.com/?x=1",
        "https://example.com/#x", "https://a..b/", "https://-a.b/", "https://a.b:0/", "https://a.b:65536/",
        "https://a.b:foo/", "https://a.b:/", "https://a.b:1:2/", "https://a.b/space here"};
    for (size_t i = 0; i < sizeof(bad_urls) / sizeof(bad_urls[0]); ++i) {
        changed = config; strcpy(changed.endpoint, bad_urls[i]); assert(!tracker_config_validate(&changed, NULL, 0));
    }
    changed = config; strcpy(changed.endpoint, "https://example.com:8443/ingest"); assert(tracker_config_validate(&changed, NULL, 0));
    changed = config; changed.parked_interval_s = UINT32_MAX; assert(!tracker_config_validate(&changed, NULL, 0));
    changed = config; strcpy(changed.access_client_secret, "secret\r\nInjected: yes");
    assert(!tracker_config_validate(&changed, error, sizeof(error)) && !strstr(error, "Injected"));
    changed = config; strcpy(changed.setup_password, "too-short"); assert(!tracker_config_validate(&changed, NULL, 0));
    changed = config; memset(changed.wifi_password, 'a', 64); changed.wifi_password[64] = 0;
    assert(tracker_config_validate(&changed, NULL, 0)); changed.wifi_password[0] = 'z'; assert(!tracker_config_validate(&changed, NULL, 0));
}

static void test_wifi_and_partial_config(void)
{
    tracker_config_t cfg, changed; tracker_config_defaults(&cfg);
    assert(cfg.version == 3 && !cfg.wifi_secondary_ssid[0] && !cfg.wifi_secondary_password[0] && !cfg.web_password[0]);
    assert(tracker_config_validate_wifi(&cfg, NULL, 0));
    assert(tracker_config_validate_stored(&cfg, NULL, 0));
    assert(!tracker_config_validate(&cfg, NULL, 0));
    strcpy(cfg.wifi_ssid, "home"); strcpy(cfg.wifi_secondary_ssid, "phone");
    strcpy(cfg.wifi_secondary_password, "phone-password");
    assert(tracker_config_validate_wifi(&cfg, NULL, 0));
    assert(tracker_config_validate_stored(&cfg, NULL, 0));
    assert(!tracker_config_validate(&cfg, NULL, 0));
    changed = cfg; changed.endpoint[0] = 0; changed.active_interval_s = 1;
    assert(tracker_config_validate_wifi(&changed, NULL, 0));
    assert(!tracker_config_validate_stored(&changed, NULL, 0));
    changed = cfg; strcpy(changed.wifi_secondary_ssid, "home");
    assert(!tracker_config_validate_wifi(&changed, NULL, 0));
    changed = cfg; changed.wifi_secondary_ssid[0] = 0;
    assert(!tracker_config_validate_wifi(&changed, NULL, 0));
    changed.wifi_secondary_password[0] = 0; assert(tracker_config_validate_wifi(&changed, NULL, 0));
    changed = cfg; strcpy(changed.wifi_secondary_password, "short");
    assert(!tracker_config_validate_wifi(&changed, NULL, 0));
    changed = cfg; memset(changed.wifi_secondary_ssid, 'x', sizeof(changed.wifi_secondary_ssid));
    assert(!tracker_config_validate_wifi(&changed, NULL, 0));
    changed = cfg; memset(changed.wifi_secondary_password, 'f', 64); changed.wifi_secondary_password[64] = 0;
    assert(tracker_config_validate_wifi(&changed, NULL, 0));
    changed.wifi_secondary_password[0] = 'g'; assert(!tracker_config_validate_wifi(&changed, NULL, 0));
    changed = cfg; strcpy(changed.wifi_secondary_ssid, "bad\nssid");
    assert(!tracker_config_validate_wifi(&changed, NULL, 0));
    changed = cfg; changed.active_interval_s = 0;
    assert(!tracker_config_validate_stored(&changed, NULL, 0));
    changed = cfg; changed.endpoint[0] = changed.device_id[0] = 0;
    assert(tracker_config_validate_stored(&changed, NULL, 0));
    assert(!tracker_config_validate(&changed, NULL, 0));
    strcpy(changed.web_password, "short"); assert(!tracker_config_validate_stored(&changed, NULL, 0));
    strcpy(changed.web_password, "web-passphrase"); assert(tracker_config_validate_stored(&changed, NULL, 0));
    memset(changed.web_password, 'p', 63); changed.web_password[63] = 0;
    assert(tracker_config_validate_stored(&changed, NULL, 0));
    changed.web_password[63] = 'p'; assert(!tracker_config_validate_stored(&changed, NULL, 0));
    changed = cfg; strcpy(changed.web_password, "bad\npassword");
    assert(!tracker_config_validate_stored(&changed, NULL, 0));
}

static void test_parser(void)
{
    tracker_fix_t fix;
    assert(tracker_parse_cgpsinfo("\r\n+CGPSINFO: 4730.0000,N,01903.0000,E,290224,123456.789,123.4,10.0,90.0\r\nOK\r\n", &fix) == TRACKER_PARSE_OK);
    assert(fabs(fix.latitude - 47.5) < 1e-9 && fabs(fix.longitude - 19.05) < 1e-9);
    assert(fabs(fix.speed_kmh - 18.52) < 1e-9 && fix.timestamp_ms == INT64_C(1709210096789));
    assert(fix.has_speed && fix.has_course && fix.has_altitude);
    assert(tracker_parse_cgpsinfo("+CGPSINFO: 0000,N,00000,E,010138,000000,,,", &fix) == TRACKER_PARSE_OK);
    assert(fix.latitude == 0 && fix.longitude == 0 && !fix.has_speed && fix.timestamp_ms == INT64_C(2145916800000));
    assert(tracker_parse_cgpsinfo("+CGPSINFO: 9000,S,18000,W,010120,000000.1,,,", &fix) == TRACKER_PARSE_OK);
    assert(fix.latitude == -90 && fix.longitude == -180 && fix.timestamp_ms == INT64_C(1577836800100));
    assert(tracker_parse_cgpsinfo("+CGPSINFO: ,,,,,,,,", &fix) == TRACKER_PARSE_NO_FIX && !fix.valid);
    const char *invalid[] = {"", "+CGPSINFO: ,,,,,,,", "+CGPSINFO: ,,,,,,,,,", "+CGPSINFO: 0000,N,,,,,,,",
        "+CGPSINFO: 4760,N,01903,E,010124,000000,0,0,0", "+CGPSINFO: 9000.1,N,01903,E,010124,000000,0,0,0",
        "+CGPSINFO: 4730,E,01903,E,010124,000000,0,0,0", "+CGPSINFO: 4730,N,18100,E,010124,000000,0,0,0",
        "+CGPSINFO: 4730,N,01903,E,290223,000000,0,0,0", "+CGPSINFO: 4730,N,01903,E,310424,000000,0,0,0",
        "+CGPSINFO: 4730,N,01903,E,010124,240000,0,0,0", "+CGPSINFO: 4730,N,01903,E,010124,000060,0,0,0",
        "+CGPSINFO: 4730,N,01903,E,010124,000000.,0,0,0", "+CGPSINFO: 4730,N,01903,E,010124,000000,NaN,0,0",
        "+CGPSINFO: 4730,N,01903,E,010124,000000,0,-1,0", "+CGPSINFO: 4730,N,01903,E,010124,000000,0,1e999,0",
        "+CGPSINFO: 4730,N,01903,E,010124,000000,0,0,361"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        memset(&fix, 1, sizeof(fix)); assert(tracker_parse_cgpsinfo(invalid[i], &fix) == TRACKER_PARSE_INVALID && !fix.valid);
    }
    char oversized[300]; memset(oversized, '1', sizeof(oversized)); memcpy(oversized, "+CGPSINFO:", 10); oversized[299] = 0;
    assert(tracker_parse_cgpsinfo(oversized, &fix) == TRACKER_PARSE_INVALID);
    assert(tracker_parse_cgpsinfo(NULL, &fix) == TRACKER_PARSE_INVALID);
}

static void test_form(void)
{
    tracker_config_t config = valid_config(); strcpy(config.device_id, "car &x=\xC3\xBC");
    tracker_sample_t sample = {0};
    assert(tracker_parse_cgpsinfo("+CGPSINFO: 4730,N,01903,E,010124,000000,100,10,90", &sample.fix) == TRACKER_PARSE_OK);
    sample.gps_fix = true; sample.ignition = true; sample.mode = TRACKER_ACTIVE; sample.network = TRACKER_NET_WIFI;
    sample.reported_at_ms = sample.fix.timestamp_ms + 1800000; sample.sequence = 123;
    char form[1024];
    int length = tracker_osmand_form(&config, &sample, form, sizeof(form));
    assert(length > 0 && length == (int)strlen(form));
    assert(strstr(form, "id=car%20%26x%3D%C3%BC") && strstr(form, "&lat=47.5000000") && strstr(form, "&speed=18.520"));
    assert(strstr(form, "&timestamp=1704067200000") && strstr(form, "&sample_id=123"));
    sample.gps_fix = false; sample.mode = TRACKER_PARKED;
    assert(tracker_osmand_form(&config, &sample, form, sizeof(form)) > 0);
    assert(!strstr(form, "&lat=") && !strstr(form, "&lon=") && !strstr(form, "&speed="));
    assert(strstr(form, "&timestamp=1704069000000") && strstr(form, "&fix_time=1704067200000") && strstr(form, "&valid=false"));
    memset(form, 'a', sizeof(form)); assert(tracker_osmand_form(&config, &sample, form, 20) == -1 && !form[0]);
    sample.fix.valid = false; assert(tracker_osmand_form(&config, &sample, form, sizeof(form)) == -1);
    assert(!strcmp(tracker_mode_name(TRACKER_PARKING_GRACE), "parking_grace"));
    assert(!strcmp(tracker_network_name(TRACKER_NET_LTE), "lte"));
}

static void test_gnss_diagnostics_parser(void)
{
    tracker_gnss_status_t status = {.poll_count = 17};
    strcpy(status.error, "existing runtime status");
    /* SIMCom example shape: GPS, GLONASS and BeiDou valid satellite counts.
     * The position is deliberately not exposed by this diagnostic parser. */
    assert(tracker_parse_cgnssinfo("\r\n+CGNSSINFO: 3,09,05,00,3113.330650,N,12121.262554,E,261026,091918.0,32.9,0.0,255.0,1.1,0.8,0.7\r\nOK\r\n", &status) == TRACKER_PARSE_OK);
    assert(status.details_available && status.fix_mode == 3);
    assert(status.gps_satellites == 9 && status.glonass_satellites == 5 && status.beidou_satellites == 0);
    assert(fabs(status.hdop - 0.8) < 1e-9);
    assert(status.poll_count == 17 && !strcmp(status.error, "existing runtime status"));
    /* Empty means unknown, not zero; this is the vendor's normal no-fix reply. */
    assert(tracker_parse_cgnssinfo("+CGNSSINFO: ,,,,,,,,,,,,,,,\r\nOK\r\n", &status) == TRACKER_PARSE_NO_FIX);
    assert(status.details_available && status.fix_mode == 0);
    assert(status.gps_satellites == -1 && status.glonass_satellites == -1 && status.beidou_satellites == -1 && status.hdop == -1);
    assert(tracker_parse_cgnssinfo("+CGNSSINFO: 2, 00 ,12,,,,,,,,,,,,0.0,", &status) == TRACKER_PARSE_OK);
    assert(status.gps_satellites == 0 && status.glonass_satellites == 12 && status.beidou_satellites == -1 && status.hdop == 0);
    const char *invalid[] = {
        "", "OK", "+CGNSSINFO: ,,,,,,,,,,,,,,", "+CGNSSINFO: ,,,,,,,,,,,,,,,,",
        "noise+CGNSSINFO: ,,,,,,,,,,,,,,,", "+CGNSSINFO: 4,,,,,,,,,,,,,,,",
        "+CGNSSINFO: 3,13,,,,,,,,,,,,,,", "+CGNSSINFO: 3,-1,,,,,,,,,,,,,,",
        "+CGNSSINFO: 3,1.5,,,,,,,,,,,,,,", "+CGNSSINFO: 3,,,,,,,,,,,,,,NaN,",
        "+CGNSSINFO: 3,,,,,,,,,,,,,,-1,", "+CGNSSINFO: 3,,,,,,,,,,,,,,1001,",
        "+CGNSSINFO: 3,1e1,,,,,,,,,,,,,,"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(tracker_parse_cgnssinfo(invalid[i], &status) == TRACKER_PARSE_INVALID);
        assert(!status.details_available && status.gps_satellites == -1 && status.hdop == -1);
    }
    assert(tracker_parse_cgnssinfo(NULL, &status) == TRACKER_PARSE_INVALID);
    assert(tracker_parse_cgnssinfo("+CGNSSINFO: ,,,,,,,,,,,,,,,", NULL) == TRACKER_PARSE_INVALID);
}

int main(void)
{
    test_config(); test_wifi_and_partial_config(); test_parser(); test_form(); test_gnss_diagnostics_parser();
    puts("tracker_core: all tests passed"); return 0;
}
