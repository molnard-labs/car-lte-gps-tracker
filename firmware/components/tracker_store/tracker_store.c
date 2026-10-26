#include "tracker_store.h"
#include "tracker_core.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#endif

#define SLOT_SIZE 96u
#define SECTOR_SIZE 4096u
#define SLOTS_PER_SECTOR (SECTOR_SIZE / SLOT_SIZE)
#define QUEUE_BYTES 0x80000u
#define SECTOR_COUNT (QUEUE_BYTES / SECTOR_SIZE)
#define PHYSICAL_SLOTS (SECTOR_COUNT * SLOTS_PER_SECTOR)
#define RECORD_MAGIC UINT32_C(0x31514B54)
#define COMMIT_MAGIC UINT32_C(0xC04D17ED)
#define META_VERSION 1u
#define CONFIG_RECORD_VERSION 3u
#define NO_SECTOR UINT32_MAX
#define CRC_OFFSET 84u
#define ACK_OFFSET 88u
#define COMMIT_OFFSET 92u

typedef struct {
    uint32_t version;
    uint32_t spare_sector;
    uint64_t next_sequence;
    uint64_t dropped;
    uint64_t drop_sequence;
    uint32_t gc_victim;
    uint32_t gc_stage;
    uint32_t crc;
} queue_meta_t;

typedef struct {
    uint32_t version;
    uint32_t previous_valid;
    uint64_t identity_generation;
    tracker_config_t current;
    tracker_config_t previous;
    uint32_t crc;
} config_record_t;

/* Exact deployed v1 layout. Do not derive this from the growing current type:
 * both snapshots and their padding participate in the legacy record CRC. */
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
} config_v1_t;

typedef struct {
    uint32_t version;
    uint32_t previous_valid;
    uint64_t identity_generation;
    config_v1_t current;
    config_v1_t previous;
    uint32_t crc;
} config_record_v1_t;

/* V2 appended the secondary profile immediately after bench_mode, before
 * the v1 struct's trailing padding. An embedded config_v1_t is not ABI-safe. */
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
} config_v2_t;

typedef struct {
    uint32_t version;
    uint32_t previous_valid;
    uint64_t identity_generation;
    config_v2_t current;
    config_v2_t previous;
    uint32_t crc;
} config_record_v2_t;

_Static_assert(sizeof(config_v1_t) == 1008, "Deployed v1 configuration ABI changed");
_Static_assert(offsetof(config_record_v1_t, crc) == 2032, "Deployed v1 record ABI changed");
_Static_assert(sizeof(config_record_t) > sizeof(config_record_v1_t), "Migration read buffer must fit v1");
_Static_assert(sizeof(config_v2_t) == 1104, "Deployed v2 configuration ABI changed");
_Static_assert(offsetof(config_v2_t, wifi_secondary_ssid) == 1005, "Deployed v2 secondary profile ABI changed");
_Static_assert(offsetof(config_record_v2_t, crc) == 2224, "Deployed v2 record ABI changed");
_Static_assert(sizeof(config_record_t) > sizeof(config_record_v2_t), "Migration read buffer must fit v2");

typedef struct {
    uint32_t version;
    uint32_t commissioned;
    uint64_t identity_generation;
    tracker_fix_t fix;
    uint32_t crc;
} runtime_record_t;

static queue_meta_t queue_meta;
static uint64_t slot_sequence[PHYSICAL_SLOTS];
static bool slot_pending[PHYSICAL_SLOTS];
static bool slot_blank[PHYSICAL_SLOTS];
static uint32_t pending_count;
static bool initialized;
static bool queue_fault;

#ifdef ESP_PLATFORM
static const esp_partition_t *queue_partition;
static SemaphoreHandle_t store_mutex;
static void lock_store(void) { if (store_mutex) xSemaphoreTake(store_mutex, portMAX_DELAY); }
static void unlock_store(void) { if (store_mutex) xSemaphoreGive(store_mutex); }

static esp_err_t kv_get(const char *key, void *data, size_t *size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("tracker", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) return err;
    err = nvs_get_blob(handle, key, data, size);
    nvs_close(handle);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}
static esp_err_t kv_put(const char *key, const void *data, size_t size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("tracker", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, key, data, size);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
static esp_err_t flash_read(size_t address, void *data, size_t size) { return esp_partition_read(queue_partition, address, data, size); }
static esp_err_t flash_write(size_t address, const void *data, size_t size) { return esp_partition_write(queue_partition, address, data, size); }
static esp_err_t flash_erase(size_t address, size_t size) { return esp_partition_erase_range(queue_partition, address, size); }
#else
static tracker_store_backend_t backend;
static void lock_store(void) {}
static void unlock_store(void) {}
static esp_err_t kv_get(const char *key, void *data, size_t *size) { return backend.kv_get(backend.context, key, data, size); }
static esp_err_t kv_put(const char *key, const void *data, size_t size) { return backend.kv_put(backend.context, key, data, size); }
static esp_err_t flash_read(size_t address, void *data, size_t size) { return backend.flash_read(backend.context, address, data, size); }
static esp_err_t flash_write(size_t address, const void *data, size_t size) { return backend.flash_write(backend.context, address, data, size); }
static esp_err_t flash_erase(size_t address, size_t size) { return backend.flash_erase(backend.context, address, size); }
void tracker_store_set_backend(const tracker_store_backend_t *replacement)
{
    if (replacement) backend = *replacement;
    else memset(&backend, 0, sizeof(backend));
    initialized = false; queue_fault = false; pending_count = 0;
    memset(slot_sequence, 0, sizeof(slot_sequence));
    memset(slot_pending, 0, sizeof(slot_pending));
    memset(slot_blank, 0, sizeof(slot_blank));
}
#endif

static uint32_t crc32(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (UINT32_C(0xEDB88320) & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void put32(uint8_t *p, uint32_t value) { for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (i * 8)); }
static uint32_t get32(const uint8_t *p) { uint32_t v = 0; for (unsigned i = 0; i < 4; ++i) v |= (uint32_t)p[i] << (i * 8); return v; }
static void put64(uint8_t *p, uint64_t value) { for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(value >> (i * 8)); }
static uint64_t get64(const uint8_t *p) { uint64_t v = 0; for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (i * 8); return v; }
static void put_double(uint8_t *p, double value) { uint64_t bits; memcpy(&bits, &value, 8); put64(p, bits); }
static double get_double(const uint8_t *p) { uint64_t bits = get64(p); double value; memcpy(&value, &bits, 8); return value; }

static bool sample_valid(const tracker_sample_t *sample)
{
    return sample && tracker_fix_is_valid(&sample->fix) && sample->reported_at_ms >= INT64_C(1577836800000) &&
        sample->reported_at_ms < INT64_C(4102444800000) && sample->mode >= TRACKER_ACTIVE && sample->mode <= TRACKER_PARKED &&
        sample->network >= TRACKER_NET_NONE && sample->network <= TRACKER_NET_LTE;
}

static void encode_record(const tracker_sample_t *sample, uint64_t sequence, uint8_t bytes[SLOT_SIZE])
{
    memset(bytes, 0, SLOT_SIZE);
    put32(bytes, RECORD_MAGIC); put32(bytes + 4, META_VERSION);
    put64(bytes + 8, sequence); put64(bytes + 16, (uint64_t)sample->reported_at_ms);
    put64(bytes + 24, (uint64_t)sample->fix.timestamp_ms);
    put_double(bytes + 32, sample->fix.latitude); put_double(bytes + 40, sample->fix.longitude);
    put_double(bytes + 48, sample->fix.altitude_m); put_double(bytes + 56, sample->fix.speed_kmh);
    put_double(bytes + 64, sample->fix.course_deg);
    bytes[72] = (uint8_t)sample->mode; bytes[73] = (uint8_t)sample->network;
    bytes[74] = sample->ignition ? 1 : 0; bytes[75] = sample->gps_fix ? 1 : 0;
    bytes[76] = sample->fix.has_altitude ? 1 : 0; bytes[77] = sample->fix.has_speed ? 1 : 0;
    bytes[78] = sample->fix.has_course ? 1 : 0;
    put32(bytes + CRC_OFFSET, crc32(bytes, CRC_OFFSET));
    put32(bytes + ACK_OFFSET, UINT32_MAX); put32(bytes + COMMIT_OFFSET, COMMIT_MAGIC);
}

static bool decode_record(const uint8_t bytes[SLOT_SIZE], tracker_sample_t *sample)
{
    if (get32(bytes) != RECORD_MAGIC || get32(bytes + 4) != META_VERSION ||
        get32(bytes + COMMIT_OFFSET) != COMMIT_MAGIC || get32(bytes + CRC_OFFSET) != crc32(bytes, CRC_OFFSET)) return false;
    memset(sample, 0, sizeof(*sample));
    sample->sequence = get64(bytes + 8); sample->reported_at_ms = (int64_t)get64(bytes + 16);
    sample->fix.timestamp_ms = (int64_t)get64(bytes + 24);
    sample->fix.latitude = get_double(bytes + 32); sample->fix.longitude = get_double(bytes + 40);
    sample->fix.altitude_m = get_double(bytes + 48); sample->fix.speed_kmh = get_double(bytes + 56);
    sample->fix.course_deg = get_double(bytes + 64);
    sample->mode = (tracker_mode_t)bytes[72]; sample->network = (tracker_network_t)bytes[73];
    for (size_t i = 74; i <= 78; ++i) if (bytes[i] > 1) return false;
    sample->ignition = bytes[74] != 0; sample->gps_fix = bytes[75] != 0;
    sample->fix.has_altitude = bytes[76] != 0; sample->fix.has_speed = bytes[77] != 0;
    sample->fix.has_course = bytes[78] != 0; sample->fix.valid = true;
    return sample->sequence != 0 && sample_valid(sample);
}

static esp_err_t save_queue_meta(void)
{
    queue_meta.crc = crc32(&queue_meta, offsetof(queue_meta_t, crc));
    esp_err_t err = kv_put("queue_meta", &queue_meta, sizeof(queue_meta));
    if (err != ESP_OK) queue_fault = true;
    return err;
}

static size_t slot_address(uint32_t slot)
{
    return (size_t)(slot / SLOTS_PER_SECTOR) * SECTOR_SIZE + (slot % SLOTS_PER_SECTOR) * SLOT_SIZE;
}

static bool all_erased(const uint8_t *bytes, size_t size)
{
    for (size_t i = 0; i < size; ++i) if (bytes[i] != 0xFF) return false;
    return true;
}

static esp_err_t write_record(uint32_t slot, const uint8_t bytes[SLOT_SIZE])
{
    esp_err_t err = flash_write(slot_address(slot), bytes, COMMIT_OFFSET);
    if (err == ESP_OK) err = flash_write(slot_address(slot) + COMMIT_OFFSET, bytes + COMMIT_OFFSET, 4);
    if (err == ESP_OK) {
        uint8_t verify[SLOT_SIZE];
        err = flash_read(slot_address(slot), verify, sizeof(verify));
        if (err == ESP_OK && memcmp(bytes, verify, sizeof(verify))) err = ESP_FAIL;
    }
    return err;
}

/* COPY keeps the original sector untouched. ERASE is persisted only after every
 * live copy is committed and verified, so recovery can safely repeat either phase. */
static esp_err_t recover_gc(void)
{
    esp_err_t err = ESP_OK;
    if (!queue_meta.gc_stage) return ESP_OK;
    if (queue_meta.gc_victim >= SECTOR_COUNT || queue_meta.gc_victim == queue_meta.spare_sector || queue_meta.gc_stage > 2)
        return ESP_ERR_INVALID_STATE;
    if (queue_meta.gc_stage == 1) {
        err = flash_erase((size_t)queue_meta.spare_sector * SECTOR_SIZE, SECTOR_SIZE);
        uint32_t destination = queue_meta.spare_sector * SLOTS_PER_SECTOR;
        for (uint32_t i = 0; err == ESP_OK && i < SLOTS_PER_SECTOR; ++i) {
            uint8_t bytes[SLOT_SIZE]; tracker_sample_t sample;
            err = flash_read(slot_address(queue_meta.gc_victim * SLOTS_PER_SECTOR + i), bytes, sizeof(bytes));
            if (err != ESP_OK) break;
            if (!decode_record(bytes, &sample)) {
                if (get32(bytes + COMMIT_OFFSET) == COMMIT_MAGIC) err = ESP_ERR_INVALID_STATE;
                continue;
            }
            if (get32(bytes + ACK_OFFSET) != 0) err = write_record(destination++, bytes);
        }
        if (err == ESP_OK) { queue_meta.gc_stage = 2; err = save_queue_meta(); }
    }
    if (err == ESP_OK && queue_meta.gc_stage == 2) {
        err = flash_erase((size_t)queue_meta.gc_victim * SECTOR_SIZE, SECTOR_SIZE);
        if (err == ESP_OK) {
            queue_meta.spare_sector = queue_meta.gc_victim;
            queue_meta.gc_victim = NO_SECTOR; queue_meta.gc_stage = 0;
            err = save_queue_meta();
        }
    }
    if (err != ESP_OK) queue_fault = true;
    return err;
}

static esp_err_t scan_queue(bool new_queue)
{
    pending_count = 0;
    memset(slot_sequence, 0, sizeof(slot_sequence)); memset(slot_pending, 0, sizeof(slot_pending));
    memset(slot_blank, 0, sizeof(slot_blank));
    for (uint32_t slot = 0; slot < PHYSICAL_SLOTS; ++slot) {
        uint8_t bytes[SLOT_SIZE]; tracker_sample_t sample;
        esp_err_t err = flash_read(slot_address(slot), bytes, sizeof(bytes));
        if (err != ESP_OK) return err;
        bool blank = all_erased(bytes, sizeof(bytes));
        if (new_queue && !blank) return ESP_ERR_INVALID_STATE;
        if (slot / SLOTS_PER_SECTOR == queue_meta.spare_sector) continue;
        slot_blank[slot] = blank;
        if (!decode_record(bytes, &sample)) {
            if (get32(bytes + COMMIT_OFFSET) == COMMIT_MAGIC) return ESP_ERR_INVALID_STATE;
            continue;
        }
        if (sample.sequence >= queue_meta.next_sequence) return ESP_ERR_INVALID_STATE;
        slot_sequence[slot] = sample.sequence;
        slot_pending[slot] = get32(bytes + ACK_OFFSET) != 0;
        if (slot_pending[slot]) ++pending_count;
    }
    return pending_count > TRACKER_QUEUE_CAPACITY ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static esp_err_t ack_slot(uint32_t slot)
{
    uint8_t zero[4] = {0}, verify[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    esp_err_t err = flash_write(slot_address(slot) + ACK_OFFSET, zero, sizeof(zero));
    if (err == ESP_OK) err = flash_read(slot_address(slot) + ACK_OFFSET, verify, sizeof(verify));
    if (err == ESP_OK && get32(verify) != 0) err = ESP_FAIL;
    if (err == ESP_OK) { slot_pending[slot] = false; --pending_count; }
    else queue_fault = true;
    return err;
}

static esp_err_t recover_drop(void)
{
    if (!queue_meta.drop_sequence) return ESP_OK;
    for (uint32_t slot = 0; slot < PHYSICAL_SLOTS; ++slot)
        if (slot_pending[slot] && slot_sequence[slot] == queue_meta.drop_sequence) {
            esp_err_t err = ack_slot(slot);
            if (err != ESP_OK) return err;
        }
    queue_meta.drop_sequence = 0;
    return save_queue_meta();
}

static esp_err_t find_writable_slot(uint32_t *slot)
{
    for (uint32_t i = 0; i < PHYSICAL_SLOTS; ++i) if (slot_blank[i]) { *slot = i; return ESP_OK; }
    uint32_t victim = NO_SECTOR, fewest = SLOTS_PER_SECTOR + 1;
    for (uint32_t sector = 0; sector < SECTOR_COUNT; ++sector) {
        if (sector == queue_meta.spare_sector) continue;
        uint32_t live = 0;
        for (uint32_t i = 0; i < SLOTS_PER_SECTOR; ++i) if (slot_pending[sector * SLOTS_PER_SECTOR + i]) ++live;
        if (live < fewest) { victim = sector; fewest = live; }
    }
    if (victim == NO_SECTOR || fewest >= SLOTS_PER_SECTOR) return ESP_ERR_INVALID_STATE;
    queue_meta.gc_victim = victim; queue_meta.gc_stage = 1;
    esp_err_t err = save_queue_meta();
    if (err == ESP_OK) err = recover_gc();
    if (err == ESP_OK) err = scan_queue(false);
    if (err != ESP_OK) return err;
    for (uint32_t i = 0; i < PHYSICAL_SLOTS; ++i) if (slot_blank[i]) { *slot = i; return ESP_OK; }
    return ESP_ERR_INVALID_STATE;
}

static bool migrate_config_v1(const config_v1_t *old, tracker_config_t *config)
{
    if (old->version != 1) return false;
    memset(config, 0, sizeof(*config));
    config->version = TRACKER_CONFIG_VERSION;
#define COPY_FIELD(field) memcpy(&config->field, &old->field, sizeof(old->field))
    COPY_FIELD(wifi_ssid); COPY_FIELD(wifi_password); COPY_FIELD(apn);
    COPY_FIELD(apn_user); COPY_FIELD(apn_password); COPY_FIELD(sim_pin);
    COPY_FIELD(endpoint); COPY_FIELD(device_id); COPY_FIELD(access_client_id);
    COPY_FIELD(access_client_secret); COPY_FIELD(setup_password); COPY_FIELD(network_mode);
    COPY_FIELD(active_interval_s); COPY_FIELD(grace_period_s); COPY_FIELD(parked_interval_s);
    COPY_FIELD(bench_mode);
#undef COPY_FIELD
    /* V1 only ever saved complete configurations. */
    return tracker_config_validate(config, NULL, 0);
}

static bool migrate_config_v2(const config_v2_t *old, tracker_config_t *config)
{
    if (old->version != 2) return false;
    memset(config, 0, sizeof(*config));
    config->version = TRACKER_CONFIG_VERSION;
#define COPY_FIELD(field) memcpy(&config->field, &old->field, sizeof(old->field))
    COPY_FIELD(wifi_ssid); COPY_FIELD(wifi_password); COPY_FIELD(apn);
    COPY_FIELD(apn_user); COPY_FIELD(apn_password); COPY_FIELD(sim_pin);
    COPY_FIELD(endpoint); COPY_FIELD(device_id); COPY_FIELD(access_client_id);
    COPY_FIELD(access_client_secret); COPY_FIELD(setup_password); COPY_FIELD(network_mode);
    COPY_FIELD(active_interval_s); COPY_FIELD(grace_period_s); COPY_FIELD(parked_interval_s);
    COPY_FIELD(bench_mode); COPY_FIELD(wifi_secondary_ssid); COPY_FIELD(wifi_secondary_password);
#undef COPY_FIELD
    /* V2 permitted partial Wi-Fi commissioning but still required its default
     * endpoint and device ID. Reject malformed legacy data without erasing it. */
    return config->endpoint[0] && config->device_id[0] && tracker_config_validate_stored(config, NULL, 0);
}

static esp_err_t load_config_record(config_record_t *record)
{
    size_t size = sizeof(*record);
    esp_err_t err = kv_get("config", record, &size);
    if (err != ESP_OK) return err;
    if (size == sizeof(config_record_v1_t) && record->version == 1) {
        config_record_v1_t *legacy = malloc(sizeof(*legacy));
        if (!legacy) return ESP_ERR_NO_MEM;
        memcpy(legacy, record, sizeof(*legacy));
        bool valid = legacy->identity_generation && legacy->previous_valid <= 1 &&
                     legacy->crc == crc32(legacy, offsetof(config_record_v1_t, crc));
        memset(record, 0, sizeof(*record));
        if (valid) {
            record->version = CONFIG_RECORD_VERSION;
            record->previous_valid = legacy->previous_valid;
            record->identity_generation = legacy->identity_generation;
            valid = migrate_config_v1(&legacy->current, &record->current) &&
                (!legacy->previous_valid || migrate_config_v1(&legacy->previous, &record->previous));
            record->crc = crc32(record, offsetof(config_record_t, crc));
        }
        memset(legacy, 0, sizeof(*legacy)); free(legacy);
        /* Migration is read-only until the next atomic config save. Queue and
         * runtime metadata retain their own version and identity generation. */
        return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (size == sizeof(config_record_v2_t) && record->version == 2) {
        config_record_v2_t *legacy = malloc(sizeof(*legacy));
        if (!legacy) return ESP_ERR_NO_MEM;
        memcpy(legacy, record, sizeof(*legacy));
        bool valid = legacy->identity_generation && legacy->previous_valid <= 1 &&
                     legacy->crc == crc32(legacy, offsetof(config_record_v2_t, crc));
        memset(record, 0, sizeof(*record));
        if (valid) {
            record->version = CONFIG_RECORD_VERSION;
            record->previous_valid = legacy->previous_valid;
            record->identity_generation = legacy->identity_generation;
            valid = migrate_config_v2(&legacy->current, &record->current) &&
                (!legacy->previous_valid || migrate_config_v2(&legacy->previous, &record->previous));
            record->crc = crc32(record, offsetof(config_record_t, crc));
        }
        memset(legacy, 0, sizeof(*legacy)); free(legacy);
        return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (size != sizeof(*record) || record->version != CONFIG_RECORD_VERSION || !record->identity_generation ||
        record->previous_valid > 1 || record->crc != crc32(record, offsetof(config_record_t, crc)) ||
        !tracker_config_validate_stored(&record->current, NULL, 0) ||
        (record->previous_valid && !tracker_config_validate_stored(&record->previous, NULL, 0))) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

static esp_err_t load_runtime(runtime_record_t *record)
{
    config_record_t config;
    esp_err_t err = load_config_record(&config);
    if (err != ESP_OK) return err;
    size_t size = sizeof(*record);
    err = kv_get("runtime", record, &size);
    if (err == ESP_ERR_NOT_FOUND) {
        memset(record, 0, sizeof(*record)); record->version = META_VERSION;
        record->identity_generation = config.identity_generation;
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    if (size != sizeof(*record) || record->version != META_VERSION || record->commissioned > 1 ||
        record->crc != crc32(record, offsetof(runtime_record_t, crc))) return ESP_ERR_INVALID_STATE;
    if (record->identity_generation != config.identity_generation) {
        memset(record, 0, sizeof(*record)); record->version = META_VERSION;
        record->identity_generation = config.identity_generation;
    }
    return ESP_OK;
}

static esp_err_t save_runtime(runtime_record_t *record)
{
    record->crc = crc32(record, offsetof(runtime_record_t, crc));
    return kv_put("runtime", record, sizeof(*record));
}

esp_err_t tracker_store_init(void)
{
#ifdef ESP_PLATFORM
    if (!store_mutex) store_mutex = xSemaphoreCreateMutex();
    if (!store_mutex) return ESP_ERR_NO_MEM;
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) return err;
    queue_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "tracker_queue");
    if (!queue_partition || queue_partition->size != QUEUE_BYTES || queue_partition->encrypted) return ESP_ERR_INVALID_SIZE;
#else
    if (!backend.kv_get || !backend.kv_put || !backend.flash_read || !backend.flash_write || !backend.flash_erase) return ESP_ERR_INVALID_ARG;
    esp_err_t err;
#endif
    lock_store();
    initialized = false; queue_fault = false; pending_count = 0;
    memset(slot_sequence, 0, sizeof(slot_sequence)); memset(slot_pending, 0, sizeof(slot_pending));
    size_t size = sizeof(queue_meta);
    err = kv_get("queue_meta", &queue_meta, &size);
    bool new_queue = err == ESP_ERR_NOT_FOUND;
    if (new_queue) {
        memset(&queue_meta, 0, sizeof(queue_meta)); queue_meta.version = META_VERSION;
        queue_meta.next_sequence = 1; queue_meta.spare_sector = SECTOR_COUNT - 1; queue_meta.gc_victim = NO_SECTOR;
    } else if (err != ESP_OK || size != sizeof(queue_meta) || queue_meta.version != META_VERSION ||
               !queue_meta.next_sequence || queue_meta.spare_sector >= SECTOR_COUNT || queue_meta.gc_stage > 2 ||
               queue_meta.crc != crc32(&queue_meta, offsetof(queue_meta_t, crc))) {
        unlock_store(); return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }
    if (!new_queue && (err = recover_gc()) != ESP_OK) { unlock_store(); return err; }
    err = scan_queue(new_queue);
    if (err == ESP_OK && !new_queue) err = recover_drop();
    if (err == ESP_OK && new_queue) err = save_queue_meta();
    if (err == ESP_OK) initialized = true;
    unlock_store(); return err;
}

esp_err_t tracker_store_load_config(tracker_config_t *config, bool *configured)
{
    if (!config || !configured) return ESP_ERR_INVALID_ARG;
    lock_store(); config_record_t record; esp_err_t err = load_config_record(&record);
    *configured = false;
    if (err == ESP_ERR_NOT_FOUND) { tracker_config_defaults(config); err = ESP_OK; }
    else if (err == ESP_OK) { *config = record.current; *configured = tracker_config_validate(config, NULL, 0); }
    unlock_store(); return err;
}

static esp_err_t save_config_locked(const tracker_config_t *config)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;
    if (!tracker_config_validate_stored(config, NULL, 0)) return ESP_ERR_INVALID_ARG;
    /* The HTTP handler already owns a full candidate config and request body.
     * Keep the double-config transaction off its bounded FreeRTOS stack. */
    config_record_t *next = calloc(1, sizeof(*next));
    if (!next) return ESP_ERR_NO_MEM;
    esp_err_t err = load_config_record(next);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) { free(next); return err; }
    if (err == ESP_OK) {
        bool identity_changed = strcmp(config->device_id, next->current.device_id) != 0 ||
                                strcmp(config->endpoint, next->current.endpoint) != 0;
        if (identity_changed && (pending_count || queue_fault || next->identity_generation == UINT64_MAX)) {
            free(next); return ESP_ERR_INVALID_STATE;
        }
        next->identity_generation += identity_changed ? 1 : 0;
        next->previous_valid = 1; next->previous = next->current;
    } else {
        if (pending_count || queue_fault) { free(next); return ESP_ERR_INVALID_STATE; }
        memset(next, 0, sizeof(*next)); next->version = CONFIG_RECORD_VERSION; next->identity_generation = 1;
    }
    next->current = *config;
    next->crc = crc32(next, offsetof(config_record_t, crc));
    err = kv_put("config", next, sizeof(*next));
    memset(next, 0, sizeof(*next)); free(next); return err;
}

esp_err_t tracker_store_save_config(const tracker_config_t *config)
{
    lock_store(); esp_err_t err = save_config_locked(config); unlock_store(); return err;
}

esp_err_t tracker_store_save_wifi(const tracker_config_t *config)
{
    if (!tracker_config_validate_wifi(config, NULL, 0)) return ESP_ERR_INVALID_ARG;
    config_record_t *record = calloc(1, sizeof(*record));
    if (!record) return ESP_ERR_NO_MEM;
    lock_store();
    esp_err_t err = load_config_record(record);
    if (err == ESP_ERR_NOT_FOUND) { tracker_config_defaults(&record->current); err = ESP_OK; }
    if (err == ESP_OK) {
#define COPY_WIFI(field) memcpy(record->current.field, config->field, sizeof(record->current.field))
        COPY_WIFI(wifi_ssid); COPY_WIFI(wifi_password);
        COPY_WIFI(wifi_secondary_ssid); COPY_WIFI(wifi_secondary_password);
#undef COPY_WIFI
        err = save_config_locked(&record->current);
    }
    unlock_store();
    memset(record, 0, sizeof(*record)); free(record); return err;
}

esp_err_t tracker_store_rollback_config(tracker_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    lock_store(); config_record_t record; esp_err_t err = load_config_record(&record);
    if (err == ESP_OK && !record.previous_valid) err = ESP_ERR_NOT_FOUND;
    if (err == ESP_OK) { err = save_config_locked(&record.previous); if (err == ESP_OK) *config = record.previous; }
    unlock_store(); return err;
}

esp_err_t tracker_store_get_setup_password(char *password, size_t size)
{
    if (!password || !size) return ESP_ERR_INVALID_ARG;
    password[0] = 0;
#ifdef ESP_PLATFORM
    esp_err_t err = nvs_flash_init_partition("factory");
    if (err != ESP_OK) return err;
    nvs_handle_t handle;
    err = nvs_open_from_partition("factory", "provision", NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    err = nvs_get_str(handle, "setup_password", password, &size);
    nvs_close(handle);
#else
    esp_err_t err = backend.factory_password ? backend.factory_password(backend.context, password, size) : ESP_ERR_NOT_FOUND;
#endif
    if (err != ESP_OK) { password[0] = 0; return err; }
    if (!memchr(password, 0, size) || strlen(password) < 12 || strlen(password) > 63) { password[0] = 0; return ESP_ERR_INVALID_STATE; }
    return ESP_OK;
}

esp_err_t tracker_store_get_sim_pin_blocked(bool *blocked)
{
    if (!blocked) return ESP_ERR_INVALID_ARG;
    lock_store(); uint32_t flag = 0; size_t size = sizeof(flag); esp_err_t err = kv_get("pin_blocked", &flag, &size);
    if (err == ESP_ERR_NOT_FOUND) err = ESP_OK;
    else if (err == ESP_OK && (size != sizeof(flag) || flag > 1)) err = ESP_ERR_INVALID_STATE;
    *blocked = err != ESP_OK || flag != 0;
    unlock_store(); return err;
}

esp_err_t tracker_store_set_sim_pin_blocked(bool blocked)
{
    lock_store(); uint32_t flag = blocked ? 1 : 0;
    esp_err_t err = kv_put("pin_blocked", &flag, sizeof(flag));
    if (err == ESP_OK) {
        uint32_t verify = UINT32_MAX; size_t size = sizeof(verify);
        err = kv_get("pin_blocked", &verify, &size);
        if (err == ESP_OK && (size != sizeof(verify) || verify != flag)) err = ESP_FAIL;
    }
    unlock_store(); return err;
}

esp_err_t tracker_store_queue_push(const tracker_sample_t *sample, uint64_t *sequence)
{
    if (!sample_valid(sample) || !sequence) return ESP_ERR_INVALID_ARG;
    lock_store();
    if (!initialized || queue_fault || queue_meta.next_sequence == UINT64_MAX) { unlock_store(); return ESP_ERR_INVALID_STATE; }
    esp_err_t err = ESP_OK;
    if (pending_count == TRACKER_QUEUE_CAPACITY) {
        uint64_t oldest = UINT64_MAX;
        for (uint32_t i = 0; i < PHYSICAL_SLOTS; ++i) if (slot_pending[i] && slot_sequence[i] < oldest) oldest = slot_sequence[i];
        if (queue_meta.dropped == UINT64_MAX) { unlock_store(); return ESP_ERR_INVALID_STATE; }
        queue_meta.drop_sequence = oldest; ++queue_meta.dropped;
        err = save_queue_meta();
        if (err == ESP_OK) err = recover_drop();
    }
    uint32_t slot = 0;
    if (err == ESP_OK) err = find_writable_slot(&slot);
    uint64_t assigned = queue_meta.next_sequence;
    if (err == ESP_OK) { ++queue_meta.next_sequence; err = save_queue_meta(); }
    uint8_t bytes[SLOT_SIZE]; encode_record(sample, assigned, bytes);
    if (err == ESP_OK) err = write_record(slot, bytes);
    if (err == ESP_OK) { slot_sequence[slot] = assigned; slot_pending[slot] = true; slot_blank[slot] = false; ++pending_count; *sequence = assigned; }
    else queue_fault = true;
    unlock_store(); return err;
}

static esp_err_t queue_peek_locked(tracker_sample_t *sample, bool newest)
{
    if (!sample) return ESP_ERR_INVALID_ARG;
    if (!initialized || queue_fault) return ESP_ERR_INVALID_STATE;
    uint32_t found = UINT32_MAX; uint64_t selected = newest ? 0 : UINT64_MAX;
    for (uint32_t i = 0; i < PHYSICAL_SLOTS; ++i)
        if (slot_pending[i] && (newest ? slot_sequence[i] > selected : slot_sequence[i] < selected)) { selected = slot_sequence[i]; found = i; }
    if (found == UINT32_MAX) return ESP_ERR_NOT_FOUND;
    uint8_t bytes[SLOT_SIZE]; esp_err_t err = flash_read(slot_address(found), bytes, sizeof(bytes));
    if (err == ESP_OK && (!decode_record(bytes, sample) || sample->sequence != selected || get32(bytes + ACK_OFFSET) == 0)) err = ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) queue_fault = true;
    return err;
}
esp_err_t tracker_store_queue_peek_oldest(tracker_sample_t *sample) { lock_store(); esp_err_t err = queue_peek_locked(sample, false); unlock_store(); return err; }
esp_err_t tracker_store_queue_peek_latest(tracker_sample_t *sample) { lock_store(); esp_err_t err = queue_peek_locked(sample, true); unlock_store(); return err; }

esp_err_t tracker_store_queue_ack(uint64_t sequence)
{
    if (!sequence) return ESP_ERR_INVALID_ARG;
    lock_store();
    if (!initialized || queue_fault) { unlock_store(); return ESP_ERR_INVALID_STATE; }
    esp_err_t err = ESP_OK;
    for (uint32_t i = 0; i < PHYSICAL_SLOTS; ++i) if (slot_sequence[i] == sequence && slot_pending[i]) { err = ack_slot(i); break; }
    unlock_store(); return err;
}

esp_err_t tracker_store_queue_stats(uint32_t *count, uint32_t *dropped)
{
    if (!count || !dropped) return ESP_ERR_INVALID_ARG;
    lock_store(); *count = pending_count; *dropped = queue_meta.dropped > UINT32_MAX ? UINT32_MAX : (uint32_t)queue_meta.dropped;
    esp_err_t err = initialized && !queue_fault ? ESP_OK : ESP_ERR_INVALID_STATE;
    unlock_store(); return err;
}

esp_err_t tracker_store_get_commissioned(bool *commissioned)
{
    if (!commissioned) return ESP_ERR_INVALID_ARG;
    lock_store(); runtime_record_t record; esp_err_t err = load_runtime(&record);
    *commissioned = err == ESP_OK && record.commissioned != 0;
    if (err == ESP_ERR_NOT_FOUND) err = ESP_OK;
    unlock_store(); return err;
}
esp_err_t tracker_store_set_commissioned(bool commissioned)
{
    lock_store(); runtime_record_t record; esp_err_t err = load_runtime(&record);
    if (err == ESP_OK) { record.commissioned = commissioned ? 1 : 0; err = save_runtime(&record); }
    unlock_store(); return err;
}
esp_err_t tracker_store_get_last_fix(tracker_fix_t *fix)
{
    if (!fix) return ESP_ERR_INVALID_ARG;
    lock_store(); runtime_record_t record; esp_err_t err = load_runtime(&record);
    if (err == ESP_OK && !tracker_fix_is_valid(&record.fix)) err = ESP_ERR_NOT_FOUND;
    if (err == ESP_OK) *fix = record.fix;
    else memset(fix, 0, sizeof(*fix));
    unlock_store(); return err;
}
esp_err_t tracker_store_set_last_fix(const tracker_fix_t *fix)
{
    if (!tracker_fix_is_valid(fix)) return ESP_ERR_INVALID_ARG;
    lock_store(); runtime_record_t record; esp_err_t err = load_runtime(&record);
    if (err == ESP_OK && (!tracker_fix_is_valid(&record.fix) || fix->timestamp_ms > record.fix.timestamp_ms)) {
        record.fix = *fix; err = save_runtime(&record);
    }
    unlock_store(); return err;
}
