#pragma once

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#endif
#include "tracker_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize NVS and the queue. Never erase configuration on an NVS error. */
esp_err_t tracker_store_init(void);
/** Load effective configuration; configured reports full commissioning readiness. */
esp_err_t tracker_store_load_config(tracker_config_t *config, bool *configured);
/** Atomically save structurally valid (possibly incomplete) configuration. */
esp_err_t tracker_store_save_config(const tracker_config_t *config);
/** Atomically merge only both Wi-Fi profiles into the latest config/defaults. */
esp_err_t tracker_store_save_wifi(const tracker_config_t *config);
/** Restore the previous validated configuration as the current configuration. */
esp_err_t tracker_store_rollback_config(tracker_config_t *config);
/** Read the separately provisioned factory setup passphrase. */
esp_err_t tracker_store_get_setup_password(char *password, size_t size);
/** Read/write the persistent SIM PIN attempt lock. */
esp_err_t tracker_store_get_sim_pin_blocked(bool *blocked);
esp_err_t tracker_store_set_sim_pin_blocked(bool blocked);
/** Append a sample to the crash-recoverable ring; assign its durable sequence. */
esp_err_t tracker_store_queue_push(const tracker_sample_t *sample, uint64_t *sequence);
/** Peek an unacknowledged sample. ESP_ERR_NOT_FOUND means an empty queue. */
esp_err_t tracker_store_queue_peek_oldest(tracker_sample_t *sample);
esp_err_t tracker_store_queue_peek_latest(tracker_sample_t *sample);
/** Acknowledge a sequence. Repeated acknowledgement is harmless. */
esp_err_t tracker_store_queue_ack(uint64_t sequence);
/** Return queue length and the persistent count of dropped samples. */
esp_err_t tracker_store_queue_stats(uint32_t *count, uint32_t *dropped);
/** Record that a real position was commissioned at the server. */
esp_err_t tracker_store_get_commissioned(bool *commissioned);
esp_err_t tracker_store_set_commissioned(bool commissioned);
/** Persist/recover the last real fix for no-fix heartbeats after reboot. */
esp_err_t tracker_store_get_last_fix(tracker_fix_t *fix);
esp_err_t tracker_store_set_last_fix(const tracker_fix_t *fix);

#ifndef ESP_PLATFORM
/** Host-test backend. kv_put must atomically replace one complete value. */
typedef struct {
    void *context;
    esp_err_t (*kv_get)(void *, const char *, void *, size_t *);
    esp_err_t (*kv_put)(void *, const char *, const void *, size_t);
    esp_err_t (*flash_read)(void *, size_t, void *, size_t);
    esp_err_t (*flash_write)(void *, size_t, const void *, size_t);
    esp_err_t (*flash_erase)(void *, size_t, size_t);
    esp_err_t (*factory_password)(void *, char *, size_t);
} tracker_store_backend_t;
/** Select a fresh host backend and discard volatile state, simulating reboot. */
void tracker_store_set_backend(const tracker_store_backend_t *backend);
#endif

#ifdef __cplusplus
}
#endif
