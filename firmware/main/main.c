#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tracker_core.h"
#include "tracker_net.h"
#include "tracker_policy.h"
#include "tracker_setup.h"
#include "tracker_store.h"
#include "tracker_output.h"

#define IGNITION_GPIO GPIO_NUM_13
#define VALID_UTC_MS INT64_C(1704067200000)
static const char *TAG = "tracker";
static atomic_bool s_ignition;
static atomic_bool s_sleep_preparing;
static SemaphoreHandle_t s_status_mutex;
static tracker_status_t s_status;
static char s_storage_error[96];
static RTC_DATA_ATTR int64_t s_heartbeat_target_ms;
static RTC_DATA_ATTR uint32_t s_rtc_marker;
#define RTC_MARKER 0x43545231u

static int64_t monotonic_ms(void) { return esp_timer_get_time() / 1000; }
static int64_t utc_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
static void storage_error(const char *operation, esp_err_t result) {
    snprintf(s_storage_error, sizeof(s_storage_error), "%s: %s", operation, esp_err_to_name(result));
    ESP_LOGE(TAG, "%s", s_storage_error);
}
static void ignition_monitor(void *arg) {
    (void)arg;
    bool candidate = gpio_get_level(IGNITION_GPIO) == 0;
    int64_t since = monotonic_ms();
    for (;;) {
        bool raw = gpio_get_level(IGNITION_GPIO) == 0;
        int64_t now = monotonic_ms();
        if (raw != candidate) { candidate = raw; since = now; }
        if (now - since >= 100 && atomic_load(&s_ignition) != candidate) {
            atomic_store(&s_ignition, candidate);
            if (candidate && atomic_load(&s_sleep_preparing)) (void)tracker_net_resume();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
static void status_read(tracker_status_t *out, void *context) {
    (void)context;
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_status_mutex);
}
static void update_status(bool configured, tracker_mode_t mode, const tracker_fix_t *last_fix,
                          int64_t last_upload, int last_http) {
    tracker_status_t status = {0};
    tracker_fix_t current;
    status.configured = configured;
    status.ignition = atomic_load(&s_ignition);
    status.mode = mode;
    status.network = tracker_net_active();
    status.gnss_fix = tracker_net_read_fix(&current) == ESP_OK;
    status.last_fix_ms = last_fix->valid ? last_fix->timestamp_ms : 0;
    status.last_upload_ms = last_upload;
    status.last_http_status = last_http;
    status.setup_active = tracker_setup_is_active();
    (void)tracker_store_queue_stats(&status.queue_count, &status.queue_dropped);
    status.sim_pin_blocked = true;
    (void)tracker_store_get_sim_pin_blocked(&status.sim_pin_blocked);
    tracker_net_get_sim_status(status.sim_status, sizeof(status.sim_status));
    tracker_net_get_error(status.error, sizeof(status.error));
    if (s_storage_error[0]) snprintf(status.error, sizeof(status.error), "%s", s_storage_error);
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status = status;
    xSemaphoreGive(s_status_mutex);
}
static void await_service(const char *operation, esp_err_t result) {
    /* Preserve NVS/queue on errors. Never "repair" by erasing user data. */
    ESP_LOGE(TAG, "%s failed: %s; service required, flash retained", operation, esp_err_to_name(result));
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
static void try_sleep(const tracker_config_t *config, const tracker_fix_t *last_fix) {
    if (last_fix->valid) {
        esp_err_t result = tracker_store_set_last_fix(last_fix);
        if (result != ESP_OK) { storage_error("Save last fix", result); return; }
    }
    atomic_store(&s_sleep_preparing, true);
    if (atomic_load(&s_ignition) || gpio_get_level(IGNITION_GPIO) == 0) {
        atomic_store(&s_sleep_preparing, false);
        return;
    }
    esp_err_t result = tracker_net_prepare_sleep(30000);
    if (result != ESP_OK || atomic_load(&s_ignition) || gpio_get_level(IGNITION_GPIO) == 0 ||
        tracker_setup_is_active()) {
        (void)tracker_net_resume();
        atomic_store(&s_sleep_preparing, false);
        return;
    }
    result = rtc_gpio_init(IGNITION_GPIO);
    if (result == ESP_OK) result = rtc_gpio_set_direction(IGNITION_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
    if (result == ESP_OK) result = rtc_gpio_pulldown_dis(IGNITION_GPIO);
    if (result == ESP_OK) result = rtc_gpio_pullup_en(IGNITION_GPIO);
    if (result == ESP_OK) result = esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    if (result == ESP_OK) result = esp_sleep_enable_ext0_wakeup(IGNITION_GPIO, 0);
    int64_t now = utc_ms();
    int64_t interval = (int64_t)config->parked_interval_s * 1000;
    int64_t delay = interval;
    if (now >= VALID_UTC_MS && s_rtc_marker == RTC_MARKER && s_heartbeat_target_ms > 0) {
        while (s_heartbeat_target_ms <= now) s_heartbeat_target_ms += interval;
        delay = s_heartbeat_target_ms - now;
    }
    if (result == ESP_OK) result = esp_sleep_enable_timer_wakeup((uint64_t)delay * 1000);
    /* ext0 is level-sensitive: an assertion after this final read immediately wakes us. */
    if (result == ESP_OK && rtc_gpio_get_level(IGNITION_GPIO) != 0 && !atomic_load(&s_ignition) &&
        !tracker_setup_is_active()) {
        if (tracker_output_prepare_sleep()) {
            ESP_LOGI(TAG, "Parked; next heartbeat in %" PRId64 " seconds", delay / 1000);
            esp_deep_sleep_start();
        }
    }
    tracker_output_cancel_sleep();
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    (void)rtc_gpio_deinit(IGNITION_GPIO);
    gpio_set_direction(IGNITION_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(IGNITION_GPIO, GPIO_PULLUP_ONLY);
    (void)tracker_net_resume();
    atomic_store(&s_sleep_preparing, false);
}
void app_main(void) {
    if (!tracker_output_init(esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED))
        await_service("Output GPIO initialization", ESP_FAIL);
    s_status_mutex = xSemaphoreCreateMutex();
    if (!s_status_mutex) await_service("Status allocation", ESP_ERR_NO_MEM);
    (void)rtc_gpio_deinit(IGNITION_GPIO);
    gpio_config_t input = {.pin_bit_mask = 1ULL << IGNITION_GPIO, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE};
    ESP_ERROR_CHECK(gpio_config(&input));
    vTaskDelay(pdMS_TO_TICKS(100));
    atomic_store(&s_ignition, gpio_get_level(IGNITION_GPIO) == 0);
    esp_err_t result = tracker_store_init();
    if (result != ESP_OK) await_service("Persistent storage", result);
    tracker_config_t config;
    bool configured = false;
    result = tracker_store_load_config(&config, &configured);
    if (result != ESP_OK) await_service("Configuration", result);
    result = tracker_net_init(&config);
    if (result != ESP_OK) await_service("Network initialization", result);
    if (xTaskCreate(ignition_monitor, "ignition", 3072, NULL, 5, NULL) != pdPASS)
        await_service("Ignition task", ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(tracker_setup_init(status_read, NULL));
    if (esp_reset_reason() == ESP_RST_POWERON) {
        result = tracker_setup_start(&config);
        if (result != ESP_OK) ESP_LOGE(TAG, "Setup unavailable: %s; check private provisioning", esp_err_to_name(result));
    }
    bool timer_wake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
    tracker_policy_t policy;
    tracker_policy_init(&policy, &config, monotonic_ms(), atomic_load(&s_ignition), timer_wake);
    tracker_fix_t last_fix = {0};
    (void)tracker_store_get_last_fix(&last_fix);
    tracker_fix_t accepted_fix = last_fix;
    bool commissioned = false;
    (void)tracker_store_get_commissioned(&commissioned);
    tracker_sample_t live = {0};
    bool live_pending = false, latest_first = true, parked_attempted = false;
    tracker_network_t previous_network = TRACKER_NET_NONE;
    int64_t last_upload = 0, next_send = 0, next_sleep = 0, next_diagnostic = 10000;
    int last_http = 0;
    unsigned failures = 0;
    bool auth_blocked = false;
    bool gnss_enabled = true;
    bool reconfigure_pending = false, config_update_pending = false, pin_retry_pending = false;
    bool apply_in_progress = false;
    int64_t parked_start = monotonic_ms();
    if (timer_wake && utc_ms() >= VALID_UTC_MS) {
        s_rtc_marker = RTC_MARKER;
        s_heartbeat_target_ms = utc_ms() + (int64_t)config.parked_interval_s * 1000;
    }
    ESP_LOGI(TAG, "Tracker started: configured=%d bench=%d", configured, config.bench_mode);
    for (;;) {
        tracker_setup_tick();
        /* Never hold this across tick: an HTTP save owns the request mutex
         * while waiting for this guard. It covers the entire old-config sample,
         * upload and acknowledgement, not merely the network call. */
        tracker_setup_tracking_lock();
        bool setup_active = tracker_setup_is_active();
        reconfigure_pending |= tracker_setup_take_session_closed();
        bool changed = false, pin_retry = false;
        if (tracker_setup_take_apply(&changed, &pin_retry)) {
            apply_in_progress = true;
            config_update_pending |= changed;
            pin_retry_pending |= pin_retry;
            reconfigure_pending = true;
        }
        /* A validated handler may be waiting for this lock or its response may
         * still be in flight. Do not sample/send/sleep before it schedules apply. */
        if (tracker_setup_is_applying() && !apply_in_progress) {
            update_status(configured, policy.mode, &last_fix, last_upload, last_http);
            tracker_setup_tracking_unlock();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (config_update_pending || pin_retry_pending || reconfigure_pending) {
            result = tracker_store_load_config(&config, &configured);
            if (result != ESP_OK) await_service("Reload configuration", result);
            result = tracker_net_reconfigure_with_pin_retry(&config, pin_retry_pending);
            reconfigure_pending = result != ESP_OK;
            if (reconfigure_pending) {
                ESP_LOGW(TAG, "Waiting for network cleanup before applying configuration");
                update_status(configured, policy.mode, &last_fix, last_upload, last_http);
                tracker_setup_tracking_unlock();
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            pin_retry_pending = false;
            if (config_update_pending) {
                tracker_policy_init(&policy, &config, monotonic_ms(), atomic_load(&s_ignition), false);
                commissioned = false;
                memset(&last_fix, 0, sizeof(last_fix));
                (void)tracker_store_get_commissioned(&commissioned);
                (void)tracker_store_get_last_fix(&last_fix);
                accepted_fix = last_fix;
                live_pending = false;
                latest_first = true;
                auth_blocked = false;
                failures = 0;
                next_send = 0;
                parked_attempted = false;
                config_update_pending = false;
            }
        }
        if (apply_in_progress) {
            if (!tracker_net_config_applied()) {
                update_status(configured, policy.mode, &last_fix, last_upload, last_http);
                tracker_setup_tracking_unlock();
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            tracker_setup_apply_complete();
            apply_in_progress = false;
        }
        int64_t now = monotonic_ms();
        /* A command received in the previous HTTPS response gets a prompt
         * measured-state report without changing the normal reporting rate. */
        if (tracker_output_take_report_due()) {
            policy.next_report_ms = now;
            parked_attempted = false;
        }
        tracker_mode_t before = policy.mode;
        bool ignition = atomic_load(&s_ignition);
        tracker_policy_update(&policy, now, ignition);
        if (before != policy.mode) {
            parked_attempted = false;
            if (policy.mode == TRACKER_PARKED) {
                parked_start = now;
                s_rtc_marker = RTC_MARKER;
                s_heartbeat_target_ms = utc_ms() >= VALID_UTC_MS ? utc_ms() + (int64_t)config.parked_interval_s * 1000 : 0;
            }
        }
        tracker_network_t network = tracker_net_active();
        if (network != TRACKER_NET_NONE && previous_network == TRACKER_NET_NONE) {
            latest_first = true;
            policy.next_report_ms = now;
        }
        previous_network = network;
        bool want_gnss = policy.mode != TRACKER_PARKED || setup_active || now - parked_start < 90000;
        if (want_gnss != gnss_enabled) {
            tracker_net_set_gnss_enabled(want_gnss);
            gnss_enabled = want_gnss;
        }
        tracker_fix_t fresh;
        bool has_fix = tracker_net_read_fix(&fresh) == ESP_OK;
        if (has_fix) last_fix = fresh;
        /* Configuration can change the device identity. Do not race a save
         * with samples or uploads using the previous in-memory identity. */
        if (configured && !tracker_setup_is_applying() && tracker_policy_report_due(&policy, now)) {
            bool nofix_allowed = commissioned && accepted_fix.valid && utc_ms() >= VALID_UTC_MS &&
                (policy.mode != TRACKER_PARKED || now - parked_start >= 90000);
            if (has_fix || nofix_allowed) {
                tracker_sample_t sample = {.fix = has_fix ? last_fix : accepted_fix, .reported_at_ms = utc_ms(), .mode = policy.mode,
                    .network = network, .ignition = ignition, .gps_fix = has_fix};
                if (has_fix) {
                    result = tracker_store_queue_push(&sample, &sample.sequence);
                    if (result != ESP_OK) storage_error("Queue append", result);
                    else { live = sample; live_pending = true; }
                } else {
                    /* Heartbeats do not consume flash position slots. */
                    sample.sequence = (uint64_t)sample.reported_at_ms;
                    live = sample;
                    live_pending = true;
                }
                tracker_policy_reported(&policy, now);
            }
        }
        /* Leave 15 seconds for the last request within the parked work budget. */
        bool can_send = policy.mode != TRACKER_PARKED || now + 15000 <= policy.work_deadline_ms;
        if (configured && !tracker_setup_is_applying() && network != TRACKER_NET_NONE && now >= next_send && !auth_blocked && can_send) {
            tracker_sample_t outgoing;
            bool from_live = live_pending;
            result = ESP_ERR_NOT_FOUND;
            if (latest_first && live_pending && !live.gps_fix) {
                result = tracker_store_queue_peek_latest(&outgoing);
                if (result == ESP_OK) from_live = false;
            }
            if (result == ESP_OK) { /* newest durable position precedes a no-fix heartbeat */ }
            else if (from_live) { outgoing = live; result = ESP_OK; }
            else if (latest_first) result = tracker_store_queue_peek_latest(&outgoing);
            else result = tracker_store_queue_peek_oldest(&outgoing);
            if (result == ESP_OK) {
                outgoing.network = network;
                if (!outgoing.gps_fix) outgoing.fix = accepted_fix;
                /* Keep historic measured/reported times on backfill; never fake a fresh fix. */
                result = tracker_net_publish(&outgoing, &last_http);
                if (policy.mode == TRACKER_PARKED && from_live) parked_attempted = true;
                if (result == ESP_OK) {
                    last_upload = utc_ms();
                    failures = 0;
                    latest_first = false;
                    if (outgoing.gps_fix) {
                        if (!last_fix.valid || outgoing.fix.timestamp_ms > last_fix.timestamp_ms) last_fix = outgoing.fix;
                        if (!accepted_fix.valid || outgoing.fix.timestamp_ms > accepted_fix.timestamp_ms) accepted_fix = outgoing.fix;
                        /* Persist accepted-fix metadata before acknowledging the
                         * durable record. A reset must not resurrect an old fix_time. */
                        result = tracker_store_set_last_fix(&outgoing.fix);
                        if (result == ESP_OK && !commissioned) {
                            result = tracker_store_set_commissioned(true);
                            if (result == ESP_OK) commissioned = true;
                        }
                        if (result == ESP_OK) result = tracker_store_queue_ack(outgoing.sequence);
                        if (result != ESP_OK) storage_error("Persist upload acknowledgement", result);
                    }
                    if (from_live) live_pending = false;
                    next_send = monotonic_ms() + 500;
                } else {
                    latest_first = true;
                    if (failures < 10) ++failures;
                    next_send = monotonic_ms() + (int64_t)tracker_retry_delay_s(failures) * 1000;
                    if (last_http == 401 || last_http == 403) auth_blocked = true;
                }
            } else if (latest_first) latest_first = false;
        }
        now = monotonic_ms();
        update_status(configured, policy.mode, &last_fix, last_upload, last_http);
        if (now >= next_diagnostic) {
            tracker_status_t diagnostic;
            status_read(&diagnostic, NULL);
            ESP_LOGI(TAG, "mode=%s network=%s fix=%d sim=%s queued=%" PRIu32 " dropped=%" PRIu32 " http=%d",
                tracker_mode_name(diagnostic.mode), tracker_network_name(diagnostic.network),
                diagnostic.gnss_fix, diagnostic.sim_status, diagnostic.queue_count,
                diagnostic.queue_dropped, diagnostic.last_http_status);
            if (diagnostic.error[0]) ESP_LOGW(TAG, "Status: %s", diagnostic.error);
            next_diagnostic = now + 60000;
        }
        if (now >= next_sleep && !tracker_output_report_pending() && tracker_policy_may_sleep(&policy, now, atomic_load(&s_ignition),
                                                        tracker_setup_is_active(), parked_attempted)) {
            try_sleep(&config, &accepted_fix);
            (void)tracker_net_resume();
            /* An aborted shutdown calls resume(), which enables GNSS again. */
            gnss_enabled = true;
            next_sleep = monotonic_ms() + 5000;
        }
        tracker_setup_tracking_unlock();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
