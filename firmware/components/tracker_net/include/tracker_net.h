#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"
#include "tracker_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes networking and a worker which exclusively owns the modem UART.
 * NVS must already be initialized. GNSS works without a SIM in Wi-Fi mode. */
esp_err_t tracker_net_init(const tracker_config_t *config);
esp_err_t tracker_net_reconfigure(const tracker_config_t *config);
/* An explicitly authorized PIN retry clears its persistent lock and replaces
 * the runtime PIN under the same PIN mutex. On failure unlocks stay paused. */
esp_err_t tracker_net_reconfigure_with_pin_retry(const tracker_config_t *config, bool retry_pin);
/* Setup must hold its tracking mutex. Begin drains the actual HTTP worker
 * (including cleanup after a caller timeout), then holds the PIN mutex through
 * storage commit. End restores prior PIN eligibility on failure; success stays
 * paused until main reloads and reconfigures. Always pair a successful begin. */
esp_err_t tracker_net_begin_config_update(bool *previous_pin_pause);
void tracker_net_end_config_update(bool committed, bool previous_pin_pause);
/* False while the worker still has queued/in-progress radio configuration. */
bool tracker_net_config_applied(void);
/* Asynchronously replace only the two STA SSID/password pairs. Setup remains
 * open, other runtime settings and the paused SIM PIN state are preserved.
 * Identical SSID/password pairs preserve the existing connection. */
esp_err_t tracker_net_apply_wifi(const tracker_config_t *config);
/* Serialize with an in-flight PIN command, then pause future PIN unlocks for
 * a validated configuration transaction without disconnecting Wi-Fi or PPP. A successful full
 * tracker_net_reconfigure() resumes PIN eligibility after application. */
esp_err_t tracker_net_pause_pin(void);
/* Associated SSID/RSSI and actual DHCP state, never stored passwords. */
void tracker_net_get_wifi_status(tracker_wifi_status_t *status);
esp_err_t tracker_net_resume(void);
/* Worker-owned GNSS/antenna transition; false also invalidates the cached fix. */
esp_err_t tracker_net_set_gnss_enabled(bool enabled);
/* The caller must not enter deep sleep unless this returns ESP_OK. On timeout
 * the worker continues shutdown; resume() cancels the requested sleep. */
esp_err_t tracker_net_prepare_sleep(uint32_t timeout_ms);

/* Nonblocking snapshots. NOT_FOUND means there is no current usable fix. */
esp_err_t tracker_net_read_fix(tracker_fix_t *fix);
/* Cached worker-owned GNSS diagnostics; this never sends an AT command. */
void tracker_net_get_gnss_status(tracker_gnss_status_t *status);
tracker_network_t tracker_net_active(void);
void tracker_net_get_error(char *buffer, size_t size);
/* Stable values: unknown, absent, pin_required, puk_required, ready, attention. */
void tracker_net_get_sim_status(char *buffer, size_t size);

/* Main/control task only; no concurrent publish/reconfigure/sleep requests.
 * A request has a 15-second caller deadline and never follows redirects.
 * Expiry cancels the worker; sleep remains blocked until its cleanup finishes.
 * HTTP status is zero when no response was received; ESP_OK requires 2xx. */
esp_err_t tracker_net_publish(const tracker_sample_t *sample, int *http_status);

/* Start returns success only after HTTPS is idle, PPP is stopped and STA has
 * stopped. After its listener is bound exclusively to the AP interface/address
 * and all routes are installed, setup calls setup_allow_wifi() to enable STA.
 * Cellular and SIM unlocking stay paused through setup. Stop the setup HTTP
 * server before setup_stop(). Accepted-socket checks alone are insufficient:
 * the listener itself must be isolated before enabling the station interface.
 * Setup start may wait up to 50 seconds for uplink teardown; failure never
 * authorizes starting its HTTP server and restores normal network eligibility. */
esp_err_t tracker_net_setup_start(const char *ssid, const char *password);
esp_err_t tracker_net_setup_allow_wifi(void);
/* APSTA -> STA preserves an existing association/lease unless CELLULAR_ONLY. */
esp_err_t tracker_net_setup_stop(void);
bool tracker_net_setup_active(void);
/* count is input capacity/output count; synchronous scan is bounded by the
 * configured per-channel dwell and returns an error during STA connection. */
esp_err_t tracker_net_scan(wifi_ap_record_t *records, uint16_t *count);

#ifdef __cplusplus
}
#endif
