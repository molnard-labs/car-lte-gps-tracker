#include "tracker_store.h"
#include "tracker_core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLASH_BYTES 0x80000u
#define SLOT_BYTES 96u
#define SLOTS_PER_SECTOR 42u
typedef struct { char key[24]; size_t size; unsigned char bytes[4096]; } fake_value_t;
typedef struct {
    unsigned char flash[FLASH_BYTES];
    fake_value_t values[8];
    unsigned failures_after, mutation_count;
    bool partial_failure, commit_failed_kv, fail_reads;
} fake_t;

static fake_value_t *value(fake_t *fake, const char *key, bool create)
{
    fake_value_t *empty = NULL;
    for (size_t i = 0; i < 8; ++i) {
        if (!strcmp(fake->values[i].key, key)) return &fake->values[i];
        if (!fake->values[i].key[0]) empty = &fake->values[i];
    }
    if (!create) return NULL;
    assert(empty && strlen(key) < sizeof(empty->key)); strcpy(empty->key, key); return empty;
}
static bool fail_mutation(fake_t *fake)
{
    ++fake->mutation_count;
    return fake->failures_after && --fake->failures_after == 0;
}
static esp_err_t fake_get(void *ctx, const char *key, void *data, size_t *size)
{
    fake_t *fake = ctx; if (fake->fail_reads) return ESP_FAIL;
    fake_value_t *entry = value(fake, key, false); if (!entry) return ESP_ERR_NOT_FOUND;
    if (*size < entry->size) return ESP_ERR_INVALID_SIZE;
    memcpy(data, entry->bytes, entry->size); *size = entry->size; return ESP_OK;
}
static esp_err_t fake_put(void *ctx, const char *key, const void *data, size_t size)
{
    fake_t *fake = ctx; bool fail = fail_mutation(fake);
    if (fail && !fake->commit_failed_kv) return ESP_FAIL;
    fake_value_t *entry = value(fake, key, true); assert(size <= sizeof(entry->bytes));
    memcpy(entry->bytes, data, size); entry->size = size; return fail ? ESP_FAIL : ESP_OK;
}
static esp_err_t fake_read(void *ctx, size_t address, void *data, size_t size)
{
    fake_t *fake = ctx; if (fake->fail_reads) return ESP_FAIL;
    assert(address <= FLASH_BYTES && size <= FLASH_BYTES - address); memcpy(data, fake->flash + address, size); return ESP_OK;
}
static esp_err_t fake_write(void *ctx, size_t address, const void *data, size_t size)
{
    fake_t *fake = ctx; const unsigned char *bytes = data;
    assert(address <= FLASH_BYTES && size <= FLASH_BYTES - address);
    bool fail = fail_mutation(fake);
    size_t written = fail ? (fake->partial_failure ? size / 2 : 0) : size;
    for (size_t i = 0; i < written; ++i) {
        assert((fake->flash[address + i] & bytes[i]) == bytes[i]);
        fake->flash[address + i] &= bytes[i];
    }
    return fail ? ESP_FAIL : ESP_OK;
}
static esp_err_t fake_erase(void *ctx, size_t address, size_t size)
{
    fake_t *fake = ctx; assert(address % 4096 == 0 && size % 4096 == 0 && address + size <= FLASH_BYTES);
    bool fail = fail_mutation(fake);
    size_t erased = fail ? (fake->partial_failure ? size / 2 : 0) : size;
    memset(fake->flash + address, 0xFF, erased); return fail ? ESP_FAIL : ESP_OK;
}
static esp_err_t fake_password(void *ctx, char *output, size_t size)
{
    (void)ctx; const char *password = "unique-test-passphrase";
    if (size <= strlen(password)) return ESP_ERR_INVALID_SIZE;
    strcpy(output, password); return ESP_OK;
}
static void bind(fake_t *fake)
{
    tracker_store_backend_t backend = {fake, fake_get, fake_put, fake_read, fake_write, fake_erase, fake_password};
    tracker_store_set_backend(&backend);
}
static void reboot(fake_t *fake)
{
    fake->failures_after = 0; fake->fail_reads = false; bind(fake); assert(tracker_store_init() == ESP_OK);
}
static fake_t *fresh(void)
{
    fake_t *fake = calloc(1, sizeof(*fake)); assert(fake); memset(fake->flash, 0xFF, sizeof(fake->flash));
    reboot(fake); return fake;
}
static tracker_config_t config(void)
{
    tracker_config_t cfg; tracker_config_defaults(&cfg);
    strcpy(cfg.wifi_ssid, "test-network"); strcpy(cfg.access_client_id, "test-id"); strcpy(cfg.access_client_secret, "test-secret");
    return cfg;
}
static tracker_sample_t sample(void)
{
    tracker_sample_t item = {0};
    assert(tracker_parse_cgpsinfo("+CGPSINFO: 4730,N,01903,E,010124,000000,100,10,90", &item.fix) == TRACKER_PARSE_OK);
    item.reported_at_ms = item.fix.timestamp_ms; item.gps_fix = true; item.ignition = true;
    item.mode = TRACKER_ACTIVE; item.network = TRACKER_NET_WIFI; return item;
}
static uint64_t push(void)
{
    tracker_sample_t item = sample(); uint64_t sequence = 0;
    assert(tracker_store_queue_push(&item, &sequence) == ESP_OK && sequence); return sequence;
}
static void stats(uint32_t expected_count, uint32_t expected_dropped)
{
    uint32_t count = 0, dropped = 0; assert(tracker_store_queue_stats(&count, &dropped) == ESP_OK);
    assert(count == expected_count && dropped == expected_dropped);
}

static void test_config_and_runtime(void)
{
    fake_t *fake = fresh(); tracker_config_t cfg = config(), loaded; bool configured = true, commissioned = true;
    assert(tracker_store_load_config(&loaded, &configured) == ESP_OK && !configured);
    assert(tracker_store_save_config(&cfg) == ESP_OK);
    assert(tracker_store_load_config(&loaded, &configured) == ESP_OK && configured && !strcmp(loaded.device_id, cfg.device_id));
    tracker_sample_t item = sample(); assert(tracker_store_set_last_fix(&item.fix) == ESP_OK);
    assert(tracker_store_set_commissioned(true) == ESP_OK);
    cfg.active_interval_s = 45; assert(tracker_store_save_config(&cfg) == ESP_OK);
    reboot(fake); tracker_fix_t recovered;
    assert(tracker_store_get_commissioned(&commissioned) == ESP_OK && commissioned);
    assert(tracker_store_get_last_fix(&recovered) == ESP_OK && recovered.timestamp_ms == item.fix.timestamp_ms);
    item.fix.timestamp_ms -= 1000; assert(tracker_store_set_last_fix(&item.fix) == ESP_OK);
    assert(tracker_store_get_last_fix(&recovered) == ESP_OK && recovered.timestamp_ms > item.fix.timestamp_ms);
    fake->failures_after = 1; cfg.active_interval_s = 60;
    assert(tracker_store_save_config(&cfg) == ESP_FAIL); reboot(fake);
    assert(tracker_store_load_config(&loaded, &configured) == ESP_OK && loaded.active_interval_s == 45);
    uint64_t sequence = push(); strcpy(cfg.device_id, "different-car");
    assert(tracker_store_save_config(&cfg) == ESP_ERR_INVALID_STATE);
    assert(tracker_store_queue_ack(sequence) == ESP_OK); assert(tracker_store_save_config(&cfg) == ESP_OK);
    assert(tracker_store_get_commissioned(&commissioned) == ESP_OK && !commissioned);
    assert(tracker_store_get_last_fix(&recovered) == ESP_ERR_NOT_FOUND);
    sequence = push(); assert(tracker_store_rollback_config(&loaded) == ESP_ERR_INVALID_STATE);
    assert(tracker_store_queue_ack(sequence) == ESP_OK); assert(tracker_store_rollback_config(&loaded) == ESP_OK);
    assert(!strcmp(loaded.device_id, "car"));
    assert(tracker_store_get_commissioned(&commissioned) == ESP_OK && !commissioned);
    assert(tracker_store_get_last_fix(&recovered) == ESP_ERR_NOT_FOUND);
    /* Changing endpoint is also an identity boundary. */
    assert(tracker_store_set_last_fix(&item.fix) == ESP_OK); assert(tracker_store_set_commissioned(true) == ESP_OK);
    strcpy(loaded.endpoint, "https://different.example/ingest"); sequence = push();
    assert(tracker_store_save_config(&loaded) == ESP_ERR_INVALID_STATE); assert(tracker_store_queue_ack(sequence) == ESP_OK);
    assert(tracker_store_save_config(&loaded) == ESP_OK); reboot(fake);
    assert(tracker_store_get_commissioned(&commissioned) == ESP_OK && !commissioned);
    assert(tracker_store_get_last_fix(&recovered) == ESP_ERR_NOT_FOUND);
    bool blocked = true; assert(tracker_store_get_sim_pin_blocked(&blocked) == ESP_OK && !blocked);
    assert(tracker_store_set_sim_pin_blocked(true) == ESP_OK); reboot(fake);
    assert(tracker_store_get_sim_pin_blocked(&blocked) == ESP_OK && blocked);
    assert(tracker_store_set_sim_pin_blocked(false) == ESP_OK); reboot(fake);
    assert(tracker_store_get_sim_pin_blocked(&blocked) == ESP_OK && !blocked);
    fake->fail_reads = true; assert(tracker_store_get_sim_pin_blocked(&blocked) == ESP_FAIL && blocked); fake->fail_reads = false;
    char password[64]; assert(tracker_store_get_setup_password(password, sizeof(password)) == ESP_OK && strlen(password) >= 12);
    assert(tracker_store_get_setup_password(password, 3) == ESP_ERR_INVALID_SIZE && !password[0]);
    fake_value_t *entry = value(fake, "config", false); assert(entry); entry->bytes[16] ^= 1;
    assert(tracker_store_load_config(&loaded, &configured) == ESP_ERR_INVALID_STATE);
    free(fake);
}

/* This fixture deliberately models the shipped v1 ABI, not the new struct. */
typedef struct {
    uint32_t version;
    char wifi_ssid[33], wifi_password[65], apn[101], apn_user[65], apn_password[65], sim_pin[9];
    char endpoint[257], device_id[65], access_client_id[129], access_client_secret[129], setup_password[64];
    tracker_network_mode_t network_mode;
    uint32_t active_interval_s, grace_period_s, parked_interval_s;
    bool bench_mode;
} legacy_config_t;
typedef struct {
    uint32_t version, previous_valid;
    uint64_t identity_generation;
    legacy_config_t current, previous;
    uint32_t crc;
} legacy_record_t;
_Static_assert(sizeof(legacy_config_t) == 1008, "v1 fixture config size");
_Static_assert(offsetof(legacy_record_t, crc) == 2032, "v1 fixture CRC offset");

typedef struct {
    uint32_t version;
    char wifi_ssid[33], wifi_password[65], apn[101], apn_user[65], apn_password[65], sim_pin[9];
    char endpoint[257], device_id[65], access_client_id[129], access_client_secret[129], setup_password[64];
    tracker_network_mode_t network_mode;
    uint32_t active_interval_s, grace_period_s, parked_interval_s;
    bool bench_mode;
    char wifi_secondary_ssid[33], wifi_secondary_password[65];
} legacy_config_v2_t;
typedef struct {
    uint32_t version, previous_valid;
    uint64_t identity_generation;
    legacy_config_v2_t current, previous;
    uint32_t crc;
} legacy_record_v2_t;
_Static_assert(sizeof(legacy_config_v2_t) == 1104, "v2 fixture config size");
_Static_assert(offsetof(legacy_config_v2_t, wifi_secondary_ssid) == 1005, "v2 fixture secondary offset");
_Static_assert(offsetof(legacy_record_v2_t, crc) == 2224, "v2 fixture CRC offset");

static uint32_t fixture_crc(const void *data, size_t size)
{
    const unsigned char *bytes = data; uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (UINT32_C(0xEDB88320) & (0u - (crc & 1u)));
    }
    return ~crc;
}

static legacy_config_t legacy_config(const tracker_config_t *cfg)
{
    legacy_config_t old = {0}; old.version = 1;
#define COPY_OLD(field) memcpy(&old.field, &cfg->field, sizeof(old.field))
    COPY_OLD(wifi_ssid); COPY_OLD(wifi_password); COPY_OLD(apn); COPY_OLD(apn_user); COPY_OLD(apn_password);
    COPY_OLD(sim_pin); COPY_OLD(endpoint); COPY_OLD(device_id); COPY_OLD(access_client_id);
    COPY_OLD(access_client_secret); COPY_OLD(setup_password); COPY_OLD(network_mode);
    COPY_OLD(active_interval_s); COPY_OLD(grace_period_s); COPY_OLD(parked_interval_s); COPY_OLD(bench_mode);
#undef COPY_OLD
    return old;
}

static legacy_config_v2_t legacy_config_v2(const tracker_config_t *cfg)
{
    legacy_config_t base = legacy_config(cfg); legacy_config_v2_t old = {0};
    memcpy(&old, &base, offsetof(legacy_config_t, bench_mode) + sizeof(base.bench_mode));
    old.version = 2;
    memcpy(old.wifi_secondary_ssid, cfg->wifi_secondary_ssid, sizeof(old.wifi_secondary_ssid));
    memcpy(old.wifi_secondary_password, cfg->wifi_secondary_password, sizeof(old.wifi_secondary_password));
    return old;
}

static void put_legacy(fake_t *fake, const tracker_config_t *current, const tracker_config_t *previous, unsigned version)
{
    legacy_record_t old = {0}; old.version = 1; old.previous_valid = 1; old.identity_generation = 1;
    fake_value_t *existing = value(fake, "config", false);
    if (existing) memcpy(&old.identity_generation, existing->bytes + 8, sizeof(old.identity_generation));
    old.current = legacy_config(current); old.previous = legacy_config(previous);
    old.crc = fixture_crc(&old, offsetof(legacy_record_t, crc));
    if (version == 1) assert(fake_put(fake, "config", &old, sizeof(old)) == ESP_OK);
    else {
        legacy_record_v2_t second = {0}; second.version = 2; second.previous_valid = 1;
        second.identity_generation = old.identity_generation;
        second.current = legacy_config_v2(current); second.previous = legacy_config_v2(previous);
        second.crc = fixture_crc(&second, offsetof(legacy_record_v2_t, crc));
        assert(fake_put(fake, "config", &second, sizeof(second)) == ESP_OK);
    }
}

static tracker_config_t fully_populated_config(void)
{
    tracker_config_t cfg = config();
    strcpy(cfg.wifi_password, "primary-password"); strcpy(cfg.apn, "test.apn");
    strcpy(cfg.apn_user, "apn-user"); strcpy(cfg.apn_password, "apn-password"); strcpy(cfg.sim_pin, "0042");
    strcpy(cfg.endpoint, "https://test.example:8443/ingest"); strcpy(cfg.device_id, "legacy-car");
    strcpy(cfg.access_client_id, "fictional-client-id"); strcpy(cfg.access_client_secret, "fictional-client-secret");
    strcpy(cfg.setup_password, "fictional-ap-passphrase"); cfg.network_mode = TRACKER_AUTO;
    cfg.active_interval_s = 42; cfg.grace_period_s = 720; cfg.parked_interval_s = 2100; cfg.bench_mode = false;
    return cfg;
}

static void test_wifi_only_save_and_rollback(void)
{
    fake_t *fake = fresh(); tracker_config_t input, expected, loaded; bool configured;
    tracker_config_defaults(&input); tracker_config_defaults(&expected);
    strcpy(input.wifi_ssid, "home"); strcpy(input.wifi_password, "home-password");
    strcpy(input.wifi_secondary_ssid, "phone"); strcpy(input.wifi_secondary_password, "phone-password");
    strcpy(expected.wifi_ssid, input.wifi_ssid); strcpy(expected.wifi_password, input.wifi_password);
    strcpy(expected.wifi_secondary_ssid, input.wifi_secondary_ssid);
    strcpy(expected.wifi_secondary_password, input.wifi_secondary_password);
    /* Unsaved form values must not be accidentally committed by Wi-Fi save. */
    input.endpoint[0] = 0; strcpy(input.device_id, "unsaved-device"); input.active_interval_s = 0;
    strcpy(input.access_client_secret, "unsaved-secret"); strcpy(input.sim_pin, "invalid");
    assert(tracker_store_save_wifi(&input) == ESP_OK); reboot(fake);
    assert(tracker_store_load_config(&loaded, &configured) == ESP_OK && !configured);
    assert(!memcmp(&loaded, &expected, sizeof(loaded)));
    assert(tracker_store_save_config(&loaded) == ESP_OK);
    input = loaded; strcpy(input.wifi_ssid, "replacement");
    assert(tracker_store_save_wifi(&input) == ESP_OK);
    assert(tracker_store_rollback_config(&loaded) == ESP_OK);
    assert(!memcmp(&loaded, &expected, sizeof(loaded)));
    assert(tracker_store_load_config(&loaded, &configured) == ESP_OK && !configured);
    strcpy(loaded.access_client_id, "client-id"); strcpy(loaded.access_client_secret, "client-secret");
    assert(tracker_store_save_config(&loaded) == ESP_OK); reboot(fake);
    assert(tracker_store_load_config(&loaded, &configured) == ESP_OK && configured);
    expected = loaded;
    tracker_sample_t item = sample(); assert(tracker_store_set_last_fix(&item.fix) == ESP_OK);
    assert(tracker_store_set_commissioned(true) == ESP_OK); assert(tracker_store_set_sim_pin_blocked(true) == ESP_OK);
    uint64_t sequence = push(); input = loaded;
    input.wifi_secondary_ssid[0] = 0; input.wifi_secondary_password[0] = 0;
    expected.wifi_secondary_ssid[0] = 0; expected.wifi_secondary_password[0] = 0;
    strcpy(input.device_id, "unsaved-other-device"); strcpy(input.apn, "unsaved-apn"); strcpy(input.sim_pin, "9999");
    strcpy(input.setup_password, "unsaved-ap-password"); strcpy(input.web_password, "unsaved-web-password");
    input.grace_period_s = 60; input.parked_interval_s = 900;
    assert(tracker_store_save_wifi(&input) == ESP_OK); reboot(fake);
    assert(tracker_store_load_config(&loaded, &configured) == ESP_OK && configured);
    assert(!memcmp(&loaded, &expected, sizeof(loaded)));
    bool flag; tracker_fix_t fix;
    assert(tracker_store_get_commissioned(&flag) == ESP_OK && flag);
    assert(tracker_store_get_sim_pin_blocked(&flag) == ESP_OK && flag);
    assert(tracker_store_get_last_fix(&fix) == ESP_OK && fix.timestamp_ms == item.fix.timestamp_ms);
    stats(1, 0); assert(tracker_store_queue_ack(sequence) == ESP_OK);
    assert(tracker_store_rollback_config(&loaded) == ESP_OK && !strcmp(loaded.wifi_secondary_ssid, "phone"));
    assert(tracker_store_get_sim_pin_blocked(&flag) == ESP_OK && flag);
    free(fake);
}

static void test_legacy_migration_and_save_failure(unsigned version)
{
    fake_t *snapshot = fresh(); tracker_config_t current = fully_populated_config(), previous = current;
    if (version == 2) {
        strcpy(current.wifi_secondary_ssid, "legacy-secondary"); strcpy(current.wifi_secondary_password, "legacy-secondary-secret");
        strcpy(previous.wifi_secondary_ssid, "previous-secondary"); strcpy(previous.wifi_secondary_password, "previous-secondary-secret");
    }
    strcpy(previous.wifi_ssid, "previous-network"); strcpy(previous.wifi_password, "previous-password");
    strcpy(previous.access_client_secret, "previous-service-secret"); previous.active_interval_s = 59;
    tracker_config_t first_identity = config(); assert(tracker_store_save_config(&first_identity) == ESP_OK);
    assert(tracker_store_save_config(&current) == ESP_OK);
    tracker_sample_t item = sample(); assert(tracker_store_set_last_fix(&item.fix) == ESP_OK);
    assert(tracker_store_set_commissioned(true) == ESP_OK); assert(tracker_store_set_sim_pin_blocked(true) == ESP_OK);
    push(); put_legacy(snapshot, &current, &previous, version);
    size_t legacy_size = version == 1 ? sizeof(legacy_record_t) : sizeof(legacy_record_v2_t);
    unsigned before = snapshot->mutation_count;
    tracker_config_t loaded; bool configured, flag;
    assert(tracker_store_load_config(&loaded, &configured) == ESP_OK && configured);
    assert(!memcmp(&loaded, &current, sizeof(loaded)));
    assert(snapshot->mutation_count == before && value(snapshot, "config", false)->size == legacy_size);
    for (unsigned mode = 0; mode < 3; ++mode) {
        fake_t *fake = malloc(sizeof(*fake)); assert(fake); memcpy(fake, snapshot, sizeof(*fake)); reboot(fake);
        tracker_config_t input = current;
        strcpy(input.wifi_secondary_ssid, "backup-network"); strcpy(input.wifi_secondary_password, "backup-password");
        strcpy(input.device_id, "must-not-overwrite"); strcpy(input.access_client_secret, "must-not-overwrite");
        if (mode) { fake->failures_after = 1; fake->commit_failed_kv = mode == 2; }
        assert(tracker_store_save_wifi(&input) == (mode ? ESP_FAIL : ESP_OK)); reboot(fake);
        tracker_config_t expected = current;
        if (mode != 1) {
            strcpy(expected.wifi_secondary_ssid, "backup-network"); strcpy(expected.wifi_secondary_password, "backup-password");
        }
        assert(tracker_store_load_config(&loaded, &configured) == ESP_OK && configured);
        assert(!memcmp(&loaded, &expected, sizeof(loaded)));
        assert((value(fake, "config", false)->size == legacy_size) == (mode == 1));
        uint64_t generation; memcpy(&generation, value(fake, "config", false)->bytes + 8, sizeof(generation));
        assert(generation == 2);
        assert(tracker_store_get_commissioned(&flag) == ESP_OK && flag);
        assert(tracker_store_get_sim_pin_blocked(&flag) == ESP_OK && flag);
        tracker_fix_t fix; assert(tracker_store_get_last_fix(&fix) == ESP_OK && fix.timestamp_ms == item.fix.timestamp_ms);
        stats(1, 0);
        assert(tracker_store_rollback_config(&loaded) == ESP_OK);
        expected = mode == 1 ? previous : current;
        assert(!memcmp(&loaded, &expected, sizeof(loaded)));
        free(fake);
    }
    /* Legacy previous is still recoverable when rollback itself is the first
     * write of v3. Neither reads nor rejected corrupt records erase data. */
    reboot(snapshot); assert(tracker_store_rollback_config(&loaded) == ESP_OK);
    assert(!memcmp(&loaded, &previous, sizeof(loaded))); stats(1, 0);
    for (unsigned corrupt = 0; corrupt < 3; ++corrupt) {
        put_legacy(snapshot, &current, &previous, version); fake_value_t *entry = value(snapshot, "config", false);
        if (corrupt == 0) entry->bytes[16] ^= 1;
        if (corrupt == 1) entry->bytes[0] = 99;
        if (corrupt == 2) --entry->size;
        before = snapshot->mutation_count;
        assert(tracker_store_load_config(&loaded, &configured) == ESP_ERR_INVALID_STATE);
        assert(tracker_store_save_wifi(&current) == ESP_ERR_INVALID_STATE);
        assert(snapshot->mutation_count == before);
    }
    free(snapshot);
}

static void test_full_partial_save_and_web_password(void)
{
    fake_t *fake = fresh(); tracker_config_t cfg, loaded; bool configured;
    tracker_config_defaults(&cfg); cfg.endpoint[0] = cfg.device_id[0] = 0;
    cfg.network_mode = TRACKER_AUTO; strcpy(cfg.web_password, "local-web-password");
    strcpy(cfg.access_client_id, "partial-client-id");
    assert(tracker_store_save_config(&cfg) == ESP_OK); reboot(fake);
    assert(tracker_store_load_config(&loaded, &configured) == ESP_OK && !configured);
    assert(!memcmp(&loaded, &cfg, sizeof(loaded)));
    assert(tracker_store_set_sim_pin_blocked(true) == ESP_OK);
    tracker_config_t invalid = cfg; strcpy(invalid.web_password, "short");
    assert(tracker_store_save_config(&invalid) == ESP_ERR_INVALID_ARG);
    invalid = cfg; strcpy(invalid.endpoint, "http://unsafe.example/");
    assert(tracker_store_save_config(&invalid) == ESP_ERR_INVALID_ARG);
    strcpy(cfg.endpoint, "https://example.com/ingest"); strcpy(cfg.device_id, "commissioned-car");
    strcpy(cfg.wifi_ssid, "home"); strcpy(cfg.apn, "internet"); strcpy(cfg.access_client_secret, "client-secret");
    assert(tracker_store_save_config(&cfg) == ESP_OK); reboot(fake);
    assert(tracker_store_load_config(&loaded, &configured) == ESP_OK && configured);
    tracker_sample_t item = sample(); assert(tracker_store_set_last_fix(&item.fix) == ESP_OK);
    assert(tracker_store_set_commissioned(true) == ESP_OK); push();
    invalid = cfg; invalid.endpoint[0] = 0;
    assert(tracker_store_save_config(&invalid) == ESP_ERR_INVALID_STATE);
    invalid = cfg; invalid.device_id[0] = 0;
    assert(tracker_store_save_config(&invalid) == ESP_ERR_INVALID_STATE);
    /* Incomplete credentials can be saved without changing queued identity,
     * runtime state or the PIN lock. Readiness disables subsequent uploads. */
    cfg.access_client_secret[0] = 0; cfg.web_password[0] = 0;
    assert(tracker_store_save_config(&cfg) == ESP_OK); reboot(fake);
    assert(tracker_store_load_config(&loaded, &configured) == ESP_OK && !configured);
    assert(!loaded.web_password[0]); bool flag; tracker_fix_t fix;
    assert(tracker_store_get_sim_pin_blocked(&flag) == ESP_OK && flag);
    assert(tracker_store_get_commissioned(&flag) == ESP_OK && flag);
    assert(tracker_store_get_last_fix(&fix) == ESP_OK && fix.timestamp_ms == item.fix.timestamp_ms);
    stats(1, 0);
    assert(tracker_store_rollback_config(&loaded) == ESP_OK);
    assert(!strcmp(loaded.web_password, "local-web-password"));
    assert(tracker_store_load_config(&loaded, &configured) == ESP_OK && configured);
    assert(tracker_store_get_sim_pin_blocked(&flag) == ESP_OK && flag); stats(1, 0);
    free(fake);
}

static void test_v2_partial_migration(void)
{
    fake_t *fake = fresh(); tracker_config_t cfg, previous, loaded; bool configured;
    tracker_config_defaults(&cfg); strcpy(cfg.wifi_ssid, "home");
    strcpy(cfg.wifi_secondary_ssid, "phone"); strcpy(cfg.wifi_secondary_password, "phone-secret");
    previous = cfg; strcpy(previous.wifi_secondary_ssid, "previous-phone");
    put_legacy(fake, &cfg, &previous, 2);
    unsigned before = fake->mutation_count;
    assert(tracker_store_load_config(&loaded, &configured) == ESP_OK && !configured);
    assert(!memcmp(&loaded, &cfg, sizeof(loaded)) && !loaded.web_password[0]);
    assert(fake->mutation_count == before);
    assert(tracker_store_rollback_config(&loaded) == ESP_OK);
    assert(!memcmp(&loaded, &previous, sizeof(loaded)));
    assert(tracker_store_load_config(&loaded, &configured) == ESP_OK && !configured);
    assert(value(fake, "config", false)->size > sizeof(legacy_record_v2_t));
    /* A valid CRC does not make invalid legacy field contents acceptable. */
    legacy_record_v2_t bad = {0}; bad.version = 2; bad.identity_generation = 1;
    bad.current = legacy_config_v2(&cfg); memset(bad.current.wifi_secondary_password, 'p', sizeof(bad.current.wifi_secondary_password));
    bad.crc = fixture_crc(&bad, offsetof(legacy_record_v2_t, crc));
    assert(fake_put(fake, "config", &bad, sizeof(bad)) == ESP_OK); before = fake->mutation_count;
    assert(tracker_store_load_config(&loaded, &configured) == ESP_ERR_INVALID_STATE);
    assert(tracker_store_save_config(&cfg) == ESP_ERR_INVALID_STATE);
    assert(fake->mutation_count == before);
    free(fake);
}

static void test_capacity_and_sparse_ack(void)
{
    fake_t *fake = fresh(); tracker_sample_t first, last;
    for (unsigned i = 0; i < TRACKER_QUEUE_CAPACITY; ++i) assert(push() == i + 1);
    stats(4096, 0); assert(push() == 4097); stats(4096, 1);
    assert(tracker_store_queue_peek_oldest(&first) == ESP_OK && first.sequence == 2);
    assert(tracker_store_queue_peek_latest(&last) == ESP_OK && last.sequence == 4097);
    assert(tracker_store_queue_ack(last.sequence) == ESP_OK); assert(tracker_store_queue_ack(last.sequence) == ESP_OK);
    assert(tracker_store_queue_ack(2000) == ESP_OK); stats(4094, 1); reboot(fake); stats(4094, 1);
    assert(tracker_store_queue_peek_oldest(&first) == ESP_OK && first.sequence == 2);
    /* Cross multiple GC rounds while retaining exact pending capacity. */
    for (unsigned i = 0; i < 6000; ++i) push();
    stats(4096, 5999); reboot(fake); stats(4096, 5999);
    uint64_t previous = 0;
    while (tracker_store_queue_peek_oldest(&first) == ESP_OK) {
        assert(first.sequence > previous); previous = first.sequence; assert(tracker_store_queue_ack(first.sequence) == ESP_OK);
    }
    stats(0, 5999); reboot(fake); stats(0, 5999); free(fake);
}

static void test_append_and_ack_power_loss(void)
{
    fake_t *snapshot = fresh(); push();
    for (unsigned mode = 0; mode < 3; ++mode) for (unsigned step = 1; step <= 3; ++step) {
        fake_t *fake = malloc(sizeof(*fake)); assert(fake); memcpy(fake, snapshot, sizeof(*fake)); reboot(fake);
        fake->failures_after = step; fake->partial_failure = mode > 0; fake->commit_failed_kv = mode == 2;
        tracker_sample_t item = sample(); uint64_t sequence = 0;
        assert(tracker_store_queue_push(&item, &sequence) != ESP_OK); reboot(fake);
        tracker_sample_t first; assert(tracker_store_queue_peek_oldest(&first) == ESP_OK && first.sequence == 1);
        uint32_t count, dropped; assert(tracker_store_queue_stats(&count, &dropped) == ESP_OK && count == 1 && dropped == 0);
        assert(push() >= 2); free(fake);
    }
    reboot(snapshot); snapshot->failures_after = 1; snapshot->partial_failure = true;
    assert(tracker_store_queue_ack(1) == ESP_FAIL); reboot(snapshot); stats(1, 0);
    assert(tracker_store_queue_ack(1) == ESP_OK); stats(0, 0); free(snapshot);
}

static void test_drop_power_loss(void)
{
    fake_t *snapshot = fresh(); for (unsigned i = 0; i < 4096; ++i) push();
    for (unsigned mode = 0; mode < 3; ++mode) for (unsigned step = 1; step <= 6; ++step) {
        fake_t *fake = malloc(sizeof(*fake)); assert(fake); memcpy(fake, snapshot, sizeof(*fake)); reboot(fake);
        fake->failures_after = step; fake->partial_failure = mode > 0; fake->commit_failed_kv = mode == 2;
        tracker_sample_t item = sample(); uint64_t sequence; assert(tracker_store_queue_push(&item, &sequence) != ESP_OK); reboot(fake);
        uint32_t count, dropped; assert(tracker_store_queue_stats(&count, &dropped) == ESP_OK);
        assert((count == 4096 && dropped == 0) || (count == 4095 && dropped == 1));
        tracker_sample_t oldest; assert(tracker_store_queue_peek_oldest(&oldest) == ESP_OK && oldest.sequence == dropped + 1);
        push(); stats(4096, 1); free(fake);
    }
    free(snapshot);
}

static void test_gc_power_loss(void)
{
    fake_t *snapshot = fresh();
    /* Fill every physical data slot but retain only each sector's first sample.
     * Thus GC must copy a live sample; no all-acknowledged sector is available. */
    for (unsigned i = 0; i < 127 * SLOTS_PER_SECTOR; ++i) {
        uint64_t sequence = push(); if (i % SLOTS_PER_SECTOR) assert(tracker_store_queue_ack(sequence) == ESP_OK);
    }
    stats(127, 0);
    for (unsigned mode = 0; mode < 3; ++mode) for (unsigned step = 1; step <= 10; ++step) {
        fake_t *fake = malloc(sizeof(*fake)); assert(fake); memcpy(fake, snapshot, sizeof(*fake)); reboot(fake);
        fake->failures_after = step; fake->partial_failure = mode > 0; fake->commit_failed_kv = mode == 2;
        tracker_sample_t item = sample(); uint64_t sequence;
        assert(tracker_store_queue_push(&item, &sequence) != ESP_OK); reboot(fake); stats(127, 0);
        assert(push() > 5334); stats(128, 0);
        for (unsigned i = 0; i < 127; ++i) {
            assert(tracker_store_queue_peek_oldest(&item) == ESP_OK && item.sequence == 1 + i * SLOTS_PER_SECTOR);
            assert(tracker_store_queue_ack(item.sequence) == ESP_OK);
        }
        stats(1, 0); free(fake);
    }
    free(snapshot);
}

static void test_corruption_is_not_erased(void)
{
    fake_t *fake = fresh(); push(); fake->flash[32] ^= 1; bind(fake);
    unsigned before = fake->mutation_count; assert(tracker_store_init() == ESP_ERR_INVALID_STATE);
    assert(fake->mutation_count == before); free(fake);
    fake = fresh(); push(); value(fake, "queue_meta", false)->key[0] = 0; bind(fake);
    before = fake->mutation_count; assert(tracker_store_init() == ESP_ERR_INVALID_STATE);
    assert(fake->mutation_count == before); free(fake);
}

int main(void)
{
    test_config_and_runtime(); test_wifi_only_save_and_rollback();
    test_legacy_migration_and_save_failure(1); test_legacy_migration_and_save_failure(2);
    test_full_partial_save_and_web_password(); test_v2_partial_migration();
    test_capacity_and_sparse_ack(); test_append_and_ack_power_loss();
    test_drop_power_loss(); test_gc_power_loss(); test_corruption_is_not_erased();
    puts("tracker_store: all tests passed (faults at every append, drop, and GC write boundary)"); return 0;
}
