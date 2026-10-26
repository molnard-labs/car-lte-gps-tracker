#include "tracker_net.h"
#include "net_policy.h"
#include "at_response.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "driver/gpio.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_modem_api.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "tracker_core.h"
#include "tracker_store.h"
#include "tracker_output.h"

#define WIFI_IP_BIT       BIT0
#define PPP_IP_BIT        BIT1
#define PAUSE_REQUEST_BIT BIT2
#define PAUSED_BIT        BIT3
#define CONFIG_DIRTY_BIT  BIT4
#define AP_ACTIVE_BIT     BIT5
#define WIFI_BUSY_BIT     BIT6
#define SCAN_BUSY_BIT     BIT7
#define HTTP_IDLE_BIT     BIT8
#define SETUP_REQUEST_BIT BIT9
#define SETUP_READY_BIT   BIT10
#define WIFI_CONFIG_DIRTY_BIT BIT11
#define SETUP_WIFI_ALLOWED_BIT BIT12
#define CONFIG_APPLYING_BIT BIT13

#define MODEM_TX 27
#define MODEM_RX 26
#define MODEM_PWRKEY GPIO_NUM_4
#define MODEM_FLIGHT GPIO_NUM_25
#define MODEM_DTR GPIO_NUM_32
#define MODEM_STATUS GPIO_NUM_34
#define AT_TIMEOUT_MS 2000u
#define GNSS_POLL_MS 2000
#define GNSS_FRESH_MS 10000
#define GNSS_DETAIL_POLL_MS 10000
#define GNSS_POWER_POLL_MS 30000
#define GNSS_DETAIL_FRESH_MS 30000
#define WIFI_FIRST_MS (2 * TRACKER_WIFI_ATTEMPT_MS + TRACKER_WIFI_PROFILE_GAP_MS)
#define LTE_RETRY_MS 30000
#define PPP_CONNECT_MS 60000
#define HTTP_TIMEOUT_MS 15000
#define EARLIEST_VALID_TIME 1704067200LL /* 2024-01-01; never skip TLS time checks. */

static const char *TAG = "tracker_net";
static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_state_mutex;
static SemaphoreHandle_t s_wifi_mutex;
static SemaphoreHandle_t s_http_mutex;
static SemaphoreHandle_t s_pin_mutex;
static TaskHandle_t s_worker;
static TaskHandle_t s_http_worker;
static tracker_config_t s_config;
static tracker_fix_t s_fix;
static tracker_gnss_status_t s_gnss_status;
static int64_t s_gnss_last_reply_ms;
static int64_t s_gnss_detail_received_ms;
static int64_t s_gnss_power_received_ms;
static int64_t s_gnss_search_started_ms;
static int64_t s_fix_received_ms;
static tracker_network_t s_active;
static char s_error[96];
static char s_sim_status[24] = "unknown";
static esp_err_t s_sleep_result;
static esp_err_t s_setup_result;
static uint32_t s_sleep_generation;
static uint32_t s_sleep_ack_generation;
static bool s_gnss_requested = true;
static bool s_pin_config_paused;
static int64_t s_wifi_bad_until_ms;
static uint8_t s_wifi_target_profile;
static char s_wifi_target_ssid[33];
static esp_netif_t *s_sta;
static esp_netif_t *s_ap;
static esp_netif_t *s_ppp;
static esp_modem_dce_t *s_dce;
static esp_http_client_handle_t s_http;
static tracker_network_t s_http_network;
static bool s_wifi_started;
static bool s_sntp_initialized;
static bool s_initialized;
static struct {
    tracker_config_t config;
    char form[2048];
    int form_length;
    tracker_network_t network;
    int64_t deadline_ms;
    bool cancel;
    esp_err_t result;
    int status;
} s_http_job;

static void http_worker(void *argument);

/* The following modem state and AT collector are exclusively worker-owned.
 * esp_modem's UART task invokes collect_at while the worker waits for it. */
static bool s_modem_ready;
static bool s_gnss_started;
static bool s_gnss_powered;
static bool s_gnss_reinitialize;
static bool s_start_pulsed;
static unsigned s_gnss_errors;
static int64_t s_next_probe_ms;
static int64_t s_next_gnss_ms;
static int64_t s_next_gnss_detail_ms;
static int64_t s_next_gnss_power_ms;
static char s_gnss_position_error[96];
static char s_gnss_power_error[96];
static tracker_wifi_policy_t s_wifi_policy;
static int64_t s_next_lte_ms;
static int64_t s_next_sim_ms;
static int64_t s_wifi_first_until_ms;
static int64_t s_ppp_started_ms;
static char s_at_response[768];

/* Raw CMUX fragments otherwise have different semantics from UART snapshots. */
#ifndef CONFIG_ESP_MODEM_USE_INFLATABLE_BUFFER_IF_NEEDED
#error "Enable ESP_MODEM_USE_INFLATABLE_BUFFER_IF_NEEDED for cumulative AT callbacks"
#endif

static int64_t monotonic_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void set_error(const char *message)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    snprintf(s_error, sizeof(s_error), "%s", message ? message : "");
    xSemaphoreGive(s_state_mutex);
}

static void get_config(tracker_config_t *config)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *config = s_config;
    xSemaphoreGive(s_state_mutex);
}

static void invalidate_fix(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    memset(&s_fix, 0, sizeof(s_fix));
    s_fix_received_ms = 0;
    xSemaphoreGive(s_state_mutex);
}

static esp_err_t collect_at(uint8_t *data, size_t length)
{
    tracker_at_result_t result = tracker_at_collect_snapshot(s_at_response, sizeof(s_at_response), data, length);
    return result == TRACKER_AT_OK ? ESP_OK : result == TRACKER_AT_ERROR ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

static esp_err_t at(const char *command, uint32_t timeout_ms)
{
    if (!s_dce) return ESP_ERR_INVALID_STATE;
    char wire[192];
    int n = snprintf(wire, sizeof(wire), "%s\r", command);
    if (n < 0 || (size_t)n >= sizeof(wire)) return ESP_ERR_INVALID_SIZE;
    memset(s_at_response, 0, sizeof(s_at_response));
    esp_err_t result = esp_modem_command(s_dce, wire, collect_at, timeout_ms);
    /* This buffer may contain a SIM PIN while echo is being disabled. */
    memset(wire, 0, sizeof(wire));
    return result;
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        if (event->esp_netif == s_sta) {
            xEventGroupSetBits(s_events, WIFI_IP_BIT);
            xEventGroupClearBits(s_events, WIFI_BUSY_BIT);
            ESP_LOGI(TAG, "STA received an IPv4 lease");
        }
    } else if (id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(s_events, WIFI_IP_BIT);
        ESP_LOGW(TAG, "STA lost its IPv4 lease");
    } else if (id == IP_EVENT_PPP_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        if (event->esp_netif == s_ppp) xEventGroupSetBits(s_events, PPP_IP_BIT);
    } else if (id == IP_EVENT_PPP_LOST_IP) {
        xEventGroupClearBits(s_events, PPP_IP_BIT);
    } else if (id == IP_EVENT_AP_STAIPASSIGNED) {
        ESP_LOGI(TAG, "Setup client received an IPv4 lease");
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, WIFI_IP_BIT | WIFI_BUSY_BIT);
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "STA disconnected (reason=%u)", (unsigned)event->reason);
    } else if (id == WIFI_EVENT_STA_STOP) {
        xEventGroupClearBits(s_events, WIFI_IP_BIT | WIFI_BUSY_BIT);
        ESP_LOGI(TAG, "STA interface stopped");
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *event = data;
        ESP_LOGI(TAG, "STA associated (channel=%u); waiting for DHCP", (unsigned)event->channel);
    } else if (id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Setup client connected to Wi-Fi");
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *event = data;
        ESP_LOGI(TAG, "Setup client disconnected (reason=%u)", (unsigned)event->reason);
    }
}

static void modem_terminal_error(esp_modem_terminal_error_t error)
{
    (void)error;
    /* No AT work from this callback: it executes on the UART reader task. */
    set_error("Modem UART error; checking connection");
    xEventGroupClearBits(s_events, PPP_IP_BIT);
}

static esp_err_t ensure_wifi_started(void)
{
    if (s_wifi_started) return ESP_OK;
    esp_err_t result = esp_wifi_start();
    if (result == ESP_OK) s_wifi_started = true;
    return result;
}

static void wifi_target(uint8_t profile, const char *ssid)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_wifi_target_profile = profile;
    snprintf(s_wifi_target_ssid, sizeof(s_wifi_target_ssid), "%s", ssid ? ssid : "");
    xSemaphoreGive(s_state_mutex);
}

static esp_err_t configure_wifi(void)
{
    if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    wifi_ap_record_t current;
    bool settling = (xEventGroupGetBits(s_events) & WIFI_BUSY_BIT) ||
                    esp_wifi_sta_get_ap_info(&current) == ESP_OK;
    ESP_LOGI(TAG, "STA configuration changed; resetting connection attempts");
    (void)esp_wifi_disconnect();
    xEventGroupClearBits(s_events, WIFI_IP_BIT | WIFI_BUSY_BIT);
    wifi_target(0, NULL);
    memset(&s_wifi_policy, 0, sizeof(s_wifi_policy));
    int64_t now = monotonic_ms();
    if (settling) s_wifi_policy.next_round_ms = now + TRACKER_WIFI_PROFILE_GAP_MS;
    xSemaphoreGive(s_wifi_mutex);
    s_wifi_first_until_ms = now + (settling ? TRACKER_WIFI_PROFILE_GAP_MS : 0) + WIFI_FIRST_MS;
    return ESP_OK;
}

/* Event bits alone can be stale during disconnect/association transitions.
 * Check the driver and the live station interface as well before replacing a
 * connection. The final check below lets a just-completed DHCP attempt win. */
static bool wifi_station_ready(void)
{
    wifi_ap_record_t record;
    esp_netif_ip_info_t ip;
    return esp_wifi_sta_get_ap_info(&record) == ESP_OK && esp_netif_is_netif_up(s_sta) &&
           esp_netif_get_ip_info(s_sta, &ip) == ESP_OK && ip.ip.addr != 0;
}

static void maintain_wifi(const tracker_config_t *config, int64_t now)
{
    EventBits_t bits = xEventGroupGetBits(s_events);
    bool setup_wifi = (bits & SETUP_WIFI_ALLOWED_BIT) != 0;
    if ((bits & (PAUSE_REQUEST_BIT | SCAN_BUSY_BIT)) ||
        ((bits & SETUP_REQUEST_BIT) && !setup_wifi) ||
        (config->network_mode == TRACKER_CELLULAR_ONLY && !setup_wifi)) return;
    if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    bits = xEventGroupGetBits(s_events);
    bool connected = wifi_station_ready();
    tracker_wifi_policy_t before = s_wifi_policy;
    uint8_t profile = tracker_wifi_policy_next(&s_wifi_policy, now,
        config->wifi_ssid[0] != '\0', config->wifi_secondary_ssid[0] != '\0',
        connected, (bits & PPP_IP_BIT) != 0);
    if (profile && wifi_station_ready()) {
        s_wifi_policy = before;
        (void)tracker_wifi_policy_next(&s_wifi_policy, now,
            config->wifi_ssid[0] != '\0', config->wifi_secondary_ssid[0] != '\0',
            true, (bits & PPP_IP_BIT) != 0);
        profile = 0;
    }
    if (!profile) {
        xSemaphoreGive(s_wifi_mutex);
        return;
    }
    if (profile == TRACKER_WIFI_DISCONNECT) {
        ESP_LOGI(TAG, "STA attempt ended; retry pause=%lld ms",
                 (long long)(s_wifi_policy.next_round_ms - now));
        (void)esp_wifi_disconnect();
        xEventGroupClearBits(s_events, WIFI_IP_BIT | WIFI_BUSY_BIT);
        wifi_target(0, NULL);
        xSemaphoreGive(s_wifi_mutex);
        return;
    }
    const char *ssid = profile == 1 ? config->wifi_ssid : config->wifi_secondary_ssid;
    const char *password = profile == 1 ? config->wifi_password : config->wifi_secondary_password;
    xEventGroupClearBits(s_events, WIFI_IP_BIT | WIFI_BUSY_BIT);
    wifi_target(profile, ssid);
    ESP_LOGI(TAG, "STA connecting (profile=%u)", (unsigned)profile);
    wifi_config_t station = {0};
    memcpy(station.sta.ssid, ssid, strnlen(ssid, sizeof(station.sta.ssid)));
    memcpy(station.sta.password, password, strnlen(password, sizeof(station.sta.password)));
    station.sta.threshold.authmode = WIFI_AUTH_OPEN;
    station.sta.pmf_cfg.capable = true;
    station.sta.pmf_cfg.required = false;
    esp_err_t result = ensure_wifi_started();
    if (result == ESP_OK) result = esp_wifi_set_config(WIFI_IF_STA, &station);
    memset(&station, 0, sizeof(station));
    if (result == ESP_OK) result = esp_wifi_connect();
    if (result == ESP_OK) xEventGroupSetBits(s_events, WIFI_BUSY_BIT);
    else s_wifi_policy.attempt_deadline_ms = monotonic_ms();
    xSemaphoreGive(s_wifi_mutex);
    if (result != ESP_OK) set_error("Wi-Fi connection attempt failed");
}

static void select_route(const tracker_config_t *config)
{
    EventBits_t bits = xEventGroupGetBits(s_events);
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool wifi_allowed = monotonic_ms() >= s_wifi_bad_until_ms;
    tracker_network_t previous = s_active;
    tracker_network_t selected = TRACKER_NET_NONE;
    if ((config->network_mode != TRACKER_CELLULAR_ONLY || (bits & SETUP_WIFI_ALLOWED_BIT)) &&
        (bits & WIFI_IP_BIT) && wifi_allowed) {
        selected = TRACKER_NET_WIFI;
    } else if (config->network_mode != TRACKER_WIFI_ONLY && (bits & PPP_IP_BIT)) {
        selected = TRACKER_NET_LTE;
    }
    s_active = selected;
    xSemaphoreGive(s_state_mutex);
    if (selected != previous && selected != TRACKER_NET_NONE) {
        (void)esp_netif_set_default_netif(selected == TRACKER_NET_WIFI ? s_sta : s_ppp);
        if (s_sntp_initialized) (void)esp_netif_sntp_start();
    }
}

static esp_err_t command_mode(void)
{
    if (!s_dce) return ESP_OK;
    esp_modem_dce_mode_t mode = esp_modem_get_mode(s_dce);
    esp_err_t result = ESP_OK;
    if (mode == ESP_MODEM_MODE_CMUX_MANUAL) {
        /* Recovery after an ESP reset with the modem still in CMUX. */
        (void)esp_modem_set_mode(s_dce, ESP_MODEM_MODE_CMUX_MANUAL_COMMAND);
        result = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_CMUX_MANUAL_EXIT);
    } else if (mode != ESP_MODEM_MODE_COMMAND) {
        /* esp_modem stops PPP and waits for its exit before closing CMUX. */
        result = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
    }
    xEventGroupClearBits(s_events, PPP_IP_BIT);
    s_ppp_started_ms = 0;
    return result;
}

static void release_modem_holds(void)
{
    /* Set intended output latches before releasing retained pad states. */
    gpio_set_level(MODEM_PWRKEY, 0);
    gpio_set_direction(MODEM_PWRKEY, GPIO_MODE_OUTPUT);
    gpio_hold_dis(MODEM_PWRKEY);
    gpio_set_level(MODEM_FLIGHT, 1);
    gpio_set_direction(MODEM_FLIGHT, GPIO_MODE_OUTPUT);
    gpio_hold_dis(MODEM_FLIGHT);
    gpio_set_level(MODEM_DTR, 0);
    gpio_set_direction(MODEM_DTR, GPIO_MODE_OUTPUT);
    gpio_hold_dis(MODEM_DTR);
}

static esp_err_t create_modem(const tracker_config_t *config)
{
    esp_modem_dte_config_t terminal = ESP_MODEM_DTE_DEFAULT_CONFIG();
    terminal.uart_config.port_num = UART_NUM_1;
    terminal.uart_config.tx_io_num = MODEM_TX;
    terminal.uart_config.rx_io_num = MODEM_RX;
    terminal.uart_config.rts_io_num = -1;
    terminal.uart_config.cts_io_num = -1;
    terminal.uart_config.baud_rate = 115200;
    terminal.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;
    terminal.uart_config.rx_buffer_size = 8192;
    terminal.uart_config.tx_buffer_size = 2048;
    terminal.dte_buffer_size = 4096;
    terminal.task_stack_size = 4096;
    terminal.task_priority = 6;
    esp_modem_dce_config_t device = ESP_MODEM_DCE_DEFAULT_CONFIG(config->apn);
    s_dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &terminal, &device, s_ppp);
    if (!s_dce) return ESP_ERR_NO_MEM;
    esp_modem_set_error_cb(s_dce, modem_terminal_error);
    return ESP_OK;
}

static void maintain_modem(const tracker_config_t *config, int64_t now)
{
    if (s_modem_ready || now < s_next_probe_ms) return;
    s_next_probe_ms = now + 5000;
    if (!s_dce && create_modem(config) != ESP_OK) {
        set_error("Cannot initialize modem driver");
        return;
    }
    if (at("AT", AT_TIMEOUT_MS) != ESP_OK) {
        /* First try recovering a running modem, not toggling its PWRKEY. */
        if (esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DETECT) == ESP_OK) {
            if (command_mode() != ESP_OK || at("AT", AT_TIMEOUT_MS) != ESP_OK) {
                set_error("Cannot restore modem command mode");
                return;
            }
        } else {
            if (!s_start_pulsed && gpio_get_level(MODEM_STATUS) == 0) {
                gpio_set_level(MODEM_PWRKEY, 1);
                vTaskDelay(pdMS_TO_TICKS(500));
                gpio_set_level(MODEM_PWRKEY, 0);
                s_start_pulsed = true;
                s_next_probe_ms = monotonic_ms() + 15000;
            }
            set_error("Modem not responding; waiting for startup");
            return;
        }
    }
    if (at("ATE0", AT_TIMEOUT_MS) != ESP_OK ||
        at("AT+CMEE=2", AT_TIMEOUT_MS) != ESP_OK ||
        at("AT+CSCLK=0", AT_TIMEOUT_MS) != ESP_OK) {
        set_error("Modem initialization command failed");
        return;
    }
    s_modem_ready = true;
    s_start_pulsed = false;
    set_error("");
}

static void snapshot_gnss_runtime(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_gnss_status.modem_ready = s_modem_ready;
    s_gnss_status.receiver_running = s_gnss_started;
    s_gnss_status.antenna_powered = s_gnss_powered;
    snprintf(s_gnss_status.error, sizeof(s_gnss_status.error), "%s",
             s_gnss_power_error[0] ? s_gnss_power_error : s_gnss_position_error);
    if (!s_gnss_status.error[0] && !strncmp(s_error, "GNSS", 4)) s_error[0] = 0;
    xSemaphoreGive(s_state_mutex);
}

static void gnss_diagnostic_error(const char *stage, esp_err_t result, char *destination, size_t size)
{
    const char *reason = result == ESP_ERR_TIMEOUT ? "timeout" :
                         result == ESP_FAIL ? "modem rejected" : esp_err_to_name(result);
    snprintf(destination, size, "%s: %s", stage, reason);
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    ++s_gnss_status.error_count;
    xSemaphoreGive(s_state_mutex);
}

static esp_err_t read_gnss_power(const char *command, const char *prefix, int *powered)
{
    esp_err_t result = at(command, AT_TIMEOUT_MS);
    if (result != ESP_OK) return result;
    const char *line = strstr(s_at_response, prefix);
    if (!line || sscanf(line + strlen(prefix), " %d", powered) != 1 ||
        (*powered != 0 && *powered != 1)) return ESP_ERR_INVALID_RESPONSE;
    return ESP_OK;
}

static esp_err_t stop_gnss(void)
{
    /* CGPS=0 may fail when GNSS is already off. Query first so the 90-second
     * acquisition cutoff and later sleep shutdown are safely repeatable. */
    int powered = -1;
    esp_err_t result = read_gnss_power("AT+CGPS?", "+CGPS:", &powered);
    if (result != ESP_OK) {
        gnss_diagnostic_error("GNSS shutdown query", result, s_gnss_power_error, sizeof(s_gnss_power_error));
        snapshot_gnss_runtime();
        return result;
    }
    if (powered == 1) result = at("AT+CGPS=0", AT_TIMEOUT_MS);
    if (result == ESP_OK) s_gnss_started = false;
    esp_err_t antenna_result = at("AT+CVAUXS=0", AT_TIMEOUT_MS);
    if (antenna_result == ESP_OK) s_gnss_powered = false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_gnss_status.receiver_state_known = result == ESP_OK;
    s_gnss_status.antenna_state_known = antenna_result == ESP_OK;
    s_gnss_status.details_available = false;
    s_gnss_search_started_ms = 0;
    if (result == ESP_OK && antenna_result == ESP_OK) s_gnss_power_received_ms = monotonic_ms();
    xSemaphoreGive(s_state_mutex);
    if (result != ESP_OK || antenna_result != ESP_OK)
        gnss_diagnostic_error("GNSS shutdown", result != ESP_OK ? result : antenna_result,
                              s_gnss_power_error, sizeof(s_gnss_power_error));
    else s_gnss_power_error[0] = 0;
    snapshot_gnss_runtime();
    return result != ESP_OK ? result : antenna_result;
}

static void gnss_command_error(const char *stage, esp_err_t result)
{
    /* Only static operation names and ESP error names, never raw AT payloads. */
    gnss_diagnostic_error(stage, result, s_gnss_power_error, sizeof(s_gnss_power_error));
    set_error(s_gnss_power_error);
    ESP_LOGW(TAG, "%s", s_gnss_power_error);
    snapshot_gnss_runtime();
    s_next_gnss_ms = monotonic_ms() + 10000;
}

static void maintain_gnss_diagnostics(int64_t now)
{
    if (now >= s_next_gnss_power_ms) {
        s_next_gnss_power_ms = now + GNSS_POWER_POLL_MS;
        int receiver = -1, antenna = -1;
        esp_err_t receiver_result = read_gnss_power("AT+CGPS?", "+CGPS:", &receiver);
        esp_err_t antenna_result = read_gnss_power("AT+CVAUXS?", "+CVAUXS:", &antenna);
        if (receiver_result == ESP_OK) s_gnss_started = receiver == 1;
        if (antenna_result == ESP_OK) s_gnss_powered = antenna == 1;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_gnss_status.receiver_state_known = receiver_result == ESP_OK;
        s_gnss_status.antenna_state_known = antenna_result == ESP_OK;
        if (receiver_result == ESP_OK && antenna_result == ESP_OK)
            s_gnss_power_received_ms = monotonic_ms();
        xSemaphoreGive(s_state_mutex);
        if (receiver_result != ESP_OK || antenna_result != ESP_OK)
            gnss_diagnostic_error("GNSS power readback", receiver_result != ESP_OK ? receiver_result : antenna_result,
                                  s_gnss_power_error, sizeof(s_gnss_power_error));
        else if (!receiver || !antenna) {
            snprintf(s_gnss_power_error, sizeof(s_gnss_power_error), "GNSS receiver or antenna supply unexpectedly off");
            /* A later owner iteration re-applies antenna/start commands. */
            s_gnss_reinitialize = true;
            invalidate_fix();
        } else s_gnss_power_error[0] = 0;
    }
    if (s_gnss_started && now >= s_next_gnss_detail_ms) {
        s_next_gnss_detail_ms = now + GNSS_DETAIL_POLL_MS;
        esp_err_t result = at("AT+CGNSSINFO", 3000);
        tracker_gnss_status_t parsed = {0};
        tracker_parse_result_t format = result == ESP_OK
            ? tracker_parse_cgnssinfo(s_at_response, &parsed) : TRACKER_PARSE_INVALID;
        char error[96] = "";
        if (result != ESP_OK || format == TRACKER_PARSE_INVALID) {
            gnss_diagnostic_error("GNSS satellite details", result == ESP_OK ? ESP_ERR_INVALID_RESPONSE : result,
                                  error, sizeof(error));
        }
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_gnss_status.details_available = result == ESP_OK && format != TRACKER_PARSE_INVALID;
        s_gnss_status.fix_mode = parsed.fix_mode;
        s_gnss_status.gps_satellites = parsed.details_available ? parsed.gps_satellites : -1;
        s_gnss_status.glonass_satellites = parsed.details_available ? parsed.glonass_satellites : -1;
        s_gnss_status.beidou_satellites = parsed.details_available ? parsed.beidou_satellites : -1;
        s_gnss_status.hdop = parsed.details_available ? parsed.hdop : -1;
        snprintf(s_gnss_status.detail_error, sizeof(s_gnss_status.detail_error), "%s", error);
        if (s_gnss_status.details_available) s_gnss_detail_received_ms = monotonic_ms();
        xSemaphoreGive(s_state_mutex);
    }
}

static void maintain_gnss(int64_t now)
{
    if (!s_modem_ready || now < s_next_gnss_ms) return;
    s_next_gnss_ms = now + GNSS_POLL_MS;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool wanted = s_gnss_requested;
    xSemaphoreGive(s_state_mutex);
    if (!wanted) {
        if (s_gnss_started || s_gnss_powered) {
            if (stop_gnss() != ESP_OK) set_error("GNSS shutdown pending");
        }
        invalidate_fix();
        snapshot_gnss_runtime();
        return;
    }
    if (!s_gnss_started || s_gnss_reinitialize) {
        /* LILYGO's index 127 means AUXVDD, not a literal CGPIO number.
         * Its SIM7600 driver programs 2.8 V, then enables the AUX supply. */
        esp_err_t result = at("AT+CVAUXV=2800", AT_TIMEOUT_MS);
        if (result != ESP_OK) {
            gnss_command_error("GNSS antenna voltage (CVAUXV)", result);
            return;
        }
        result = at("AT+CVAUXS=1", AT_TIMEOUT_MS);
        if (result != ESP_OK) {
            gnss_command_error("GNSS antenna power (CVAUXS)", result);
            return;
        }
        s_gnss_powered = true;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_gnss_status.antenna_state_known = true;
        xSemaphoreGive(s_state_mutex);
        result = at("AT+CGPS?", AT_TIMEOUT_MS);
        if (result != ESP_OK) {
            gnss_command_error("GNSS power query (CGPS?)", result);
            return;
        }
        const char *state_line = strstr(s_at_response, "+CGPS:");
        int powered = -1;
        if (!state_line || sscanf(state_line, "+CGPS: %d", &powered) != 1 ||
            (powered != 0 && powered != 1)) {
            gnss_command_error("GNSS power query format", ESP_ERR_INVALID_RESPONSE);
            return;
        }
        if (powered == 0 && (result = at("AT+CGPS=1", AT_TIMEOUT_MS)) != ESP_OK) {
            gnss_command_error("GNSS standalone start (CGPS)", result);
            return;
        }
        s_gnss_started = true;
        s_gnss_reinitialize = false;
        s_gnss_power_error[0] = 0;
        s_gnss_position_error[0] = 0;
        s_next_gnss_power_ms = s_next_gnss_detail_ms = 0;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_gnss_status.receiver_state_known = true;
        s_gnss_search_started_ms = monotonic_ms();
        xSemaphoreGive(s_state_mutex);
        set_error("");
        ESP_LOGI(TAG, "GNSS antenna powered; standalone receiver running");
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    ++s_gnss_status.poll_count;
    xSemaphoreGive(s_state_mutex);
    esp_err_t poll_result = at("AT+CGPSINFO", 3000);
    if (poll_result != ESP_OK) {
        invalidate_fix();
        set_error("GNSS query failed");
        gnss_diagnostic_error("GNSS position query", poll_result, s_gnss_position_error, sizeof(s_gnss_position_error));
        if (++s_gnss_errors >= 3) {
            s_modem_ready = false;
            s_gnss_started = false;
            s_gnss_errors = 0;
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_gnss_status.receiver_state_known = false;
            s_gnss_status.antenna_state_known = false;
            xSemaphoreGive(s_state_mutex);
        }
        snapshot_gnss_runtime();
        return;
    }
    s_gnss_errors = 0;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_gnss_last_reply_ms = monotonic_ms();
    xSemaphoreGive(s_state_mutex);
    tracker_fix_t fix = {0};
    tracker_parse_result_t parsed = tracker_parse_cgpsinfo(s_at_response, &fix);
    if (parsed != TRACKER_PARSE_OK) {
        invalidate_fix();
        if (parsed == TRACKER_PARSE_NO_FIX) s_gnss_position_error[0] = 0;
        else {
            gnss_diagnostic_error("GNSS position format", ESP_ERR_INVALID_RESPONSE,
                                  s_gnss_position_error, sizeof(s_gnss_position_error));
        }
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (!s_gnss_search_started_ms) s_gnss_search_started_ms = monotonic_ms();
        xSemaphoreGive(s_state_mutex);
        maintain_gnss_diagnostics(monotonic_ms());
        snapshot_gnss_runtime();
        return;
    }
    int64_t utc_now = (int64_t)time(NULL);
    if (utc_now >= EARLIEST_VALID_TIME &&
        (fix.timestamp_ms / 1000 < utc_now - 30 || fix.timestamp_ms / 1000 > utc_now + 10)) {
        invalidate_fix();
        set_error("GNSS returned an old or future fix");
        gnss_diagnostic_error("GNSS fix timestamp", ESP_ERR_INVALID_RESPONSE,
                              s_gnss_position_error, sizeof(s_gnss_position_error));
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (!s_gnss_search_started_ms) s_gnss_search_started_ms = monotonic_ms();
        xSemaphoreGive(s_state_mutex);
        maintain_gnss_diagnostics(monotonic_ms());
        snapshot_gnss_runtime();
        return;
    }
    s_gnss_position_error[0] = 0;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    /* Acquisition can be canceled while the modem command is in flight. */
    if (s_gnss_requested) {
        s_fix = fix;
        s_fix_received_ms = monotonic_ms();
        s_gnss_search_started_ms = 0;
    }
    xSemaphoreGive(s_state_mutex);
    if (time(NULL) < EARLIEST_VALID_TIME && fix.timestamp_ms / 1000 >= EARLIEST_VALID_TIME) {
        struct timeval tv = {.tv_sec = fix.timestamp_ms / 1000,
                             .tv_usec = (fix.timestamp_ms % 1000) * 1000};
        settimeofday(&tv, NULL);
    }
    maintain_gnss_diagnostics(monotonic_ms());
    snapshot_gnss_runtime();
}

static esp_err_t query_sim(void)
{
    esp_err_t result = at("AT+CPIN?", AT_TIMEOUT_MS);
    const char *status = "unknown";
    if (strstr(s_at_response, "+CPIN: READY")) status = "ready";
    else if (strstr(s_at_response, "+CPIN: SIM PIN")) status = "pin_required";
    else if (strstr(s_at_response, "+CPIN: SIM PUK")) status = "puk_required";
    else if (strstr(s_at_response, "+CPIN:")) status = "attention";
    else if (strstr(s_at_response, "+CME ERROR: 10\r") ||
             strstr(s_at_response, "+CME ERROR: 10\n") ||
             strstr(s_at_response, "SIM not inserted")) status = "absent";
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    snprintf(s_sim_status, sizeof(s_sim_status), "%s", status);
    xSemaphoreGive(s_state_mutex);
    s_next_sim_ms = monotonic_ms() + 10000;
    return result;
}

static bool sim_ready(const tracker_config_t *config)
{
    if (query_sim() != ESP_OK) {
        set_error("SIM absent or unavailable");
        return false;
    }
    if (strstr(s_at_response, "+CPIN: READY")) {
        bool blocked = false;
        if (tracker_store_get_sim_pin_blocked(&blocked) != ESP_OK ||
            (blocked && tracker_store_set_sim_pin_blocked(false) != ESP_OK)) {
            set_error("Cannot persist SIM PIN status");
            return false;
        }
        return true;
    }
    if (!strstr(s_at_response, "+CPIN: SIM PIN")) {
        set_error("SIM requires attention (possibly PUK)");
        return false;
    }
    /* Serialize unlock with AP activation and configuration replacement. An old
     * worker snapshot must never consume an attempt after the PIN is changed. */
    if (xSemaphoreTake(s_pin_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool paused = s_pin_config_paused || strcmp(config->sim_pin, s_config.sim_pin) != 0;
    xSemaphoreGive(s_state_mutex);
    if (paused || tracker_net_setup_active()) {
        xSemaphoreGive(s_pin_mutex);
        set_error("SIM PIN unlock paused during setup");
        return false;
    }
    bool blocked = true;
    if (tracker_store_get_sim_pin_blocked(&blocked) != ESP_OK || blocked) {
        xSemaphoreGive(s_pin_mutex);
        set_error("SIM PIN retry blocked; review setup");
        return false;
    }
    if (!config->sim_pin[0]) {
        xSemaphoreGive(s_pin_mutex);
        set_error("SIM PIN required; configure it in setup");
        return false;
    }
    /* Commit BEFORE transmitting: interruption cannot consume another attempt. */
    if (tracker_store_set_sim_pin_blocked(true) != ESP_OK) {
        xSemaphoreGive(s_pin_mutex);
        set_error("Cannot persist SIM PIN attempt; unlock skipped");
        return false;
    }
    char command[48];
    snprintf(command, sizeof(command), "AT+CPIN=\"%s\"", config->sim_pin);
    esp_err_t result = at(command, AT_TIMEOUT_MS);
    memset(command, 0, sizeof(command));
    memset(s_at_response, 0, sizeof(s_at_response));
    xSemaphoreGive(s_pin_mutex);
    if (result != ESP_OK) set_error("SIM PIN rejected; automatic retry blocked");
    /* Confirm READY on the next poll before clearing the persistent latch. */
    return false;
}

static bool registered_lte(void)
{
    if (at("AT+CEREG?", AT_TIMEOUT_MS) != ESP_OK) return false;
    const char *line = strstr(s_at_response, "+CEREG:");
    int mode = 0, registration = 0;
    return line && sscanf(line, "+CEREG: %d,%d", &mode, &registration) == 2 &&
           (registration == 1 || registration == 5);
}

static void maintain_lte(const tracker_config_t *config, int64_t now)
{
    EventBits_t bits = xEventGroupGetBits(s_events);
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool wifi_healthy = now >= s_wifi_bad_until_ms;
    xSemaphoreGive(s_state_mutex);
    bool want_lte = config->network_mode == TRACKER_CELLULAR_ONLY ||
                    (config->network_mode == TRACKER_AUTO &&
                     (!(bits & WIFI_IP_BIT) || !wifi_healthy) &&
                     ((!config->wifi_ssid[0] && !config->wifi_secondary_ssid[0]) ||
                      now >= s_wifi_first_until_ms));
    if (!want_lte) {
        if (s_ppp_started_ms) (void)command_mode();
        return;
    }
    if (!s_modem_ready || now < s_next_lte_ms) return;
    if (bits & PPP_IP_BIT) return;
    if (s_ppp_started_ms) {
        if (now - s_ppp_started_ms >= PPP_CONNECT_MS) {
            (void)command_mode();
            s_next_lte_ms = now + LTE_RETRY_MS;
            set_error("Cellular IP acquisition timed out");
        }
        return;
    }
    s_next_lte_ms = now + 5000;
    if (!sim_ready(config)) return;
    if (!registered_lte()) {
        set_error("Waiting for LTE network registration");
        return;
    }
    if (!config->apn[0]) {
        set_error("Cellular APN is not configured");
        return;
    }
    if (esp_modem_set_apn(s_dce, config->apn) != ESP_OK) {
        set_error("Could not configure cellular APN");
        return;
    }
    esp_netif_auth_type_t auth = config->apn_user[0]
        ? (NETIF_PPP_AUTHTYPE_PAP | NETIF_PPP_AUTHTYPE_CHAP) : NETIF_PPP_AUTHTYPE_NONE;
    if (esp_netif_ppp_set_auth(s_ppp, auth, config->apn_user, config->apn_password) != ESP_OK) {
        set_error("Could not configure PPP authentication");
        return;
    }
    /* CMUX has one AT channel for GNSS and one PPP channel. No raw UART reads. */
    if (esp_modem_set_mode(s_dce, ESP_MODEM_MODE_CMUX) != ESP_OK) {
        (void)command_mode();
        s_next_lte_ms = monotonic_ms() + LTE_RETRY_MS;
        set_error("CMUX/PPP startup failed");
        return;
    }
    s_ppp_started_ms = monotonic_ms();
}

static esp_err_t shutdown_for_sleep(void)
{
    /* Control task closed HTTP before requesting this transition. */
    esp_err_t result = command_mode();
    if (result != ESP_OK) {
        set_error("PPP/CMUX shutdown failed; deep sleep not ready");
        return result;
    }
    if (!(xEventGroupGetBits(s_events) & PAUSE_REQUEST_BIT)) return ESP_ERR_INVALID_STATE;
    if (!s_modem_ready) {
        set_error("Modem state unknown; deep sleep not ready");
        return ESP_ERR_INVALID_STATE;
    }
    if (stop_gnss() != ESP_OK ||
        at("AT+CSCLK=1", AT_TIMEOUT_MS) != ESP_OK) {
        set_error("Modem sleep command failed");
        return ESP_FAIL;
    }
    s_gnss_started = false;
    s_gnss_powered = false;
    invalidate_fix();
    if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!(xEventGroupGetBits(s_events) & PAUSE_REQUEST_BIT) || tracker_net_setup_active()) {
        xSemaphoreGive(s_wifi_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_wifi_started) {
        result = esp_wifi_stop();
        if (result == ESP_OK) s_wifi_started = false;
    }
    xSemaphoreGive(s_wifi_mutex);
    if (result != ESP_OK) return result;
    if (!(xEventGroupGetBits(s_events) & PAUSE_REQUEST_BIT)) return ESP_ERR_INVALID_STATE;
    gpio_set_level(MODEM_PWRKEY, 0);
    gpio_set_level(MODEM_FLIGHT, 1);
    gpio_set_level(MODEM_DTR, 1);
    gpio_hold_en(MODEM_PWRKEY);
    gpio_hold_en(MODEM_FLIGHT);
    gpio_hold_en(MODEM_DTR);
    gpio_deep_sleep_hold_en();
    s_modem_ready = false;
    snapshot_gnss_runtime();
    xEventGroupClearBits(s_events, WIFI_IP_BIT | PPP_IP_BIT | WIFI_BUSY_BIT);
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_active = TRACKER_NET_NONE;
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

static esp_err_t pause_uplinks_for_setup(void)
{
    /* This runs on the modem owner. The HTTP server cannot start until both
     * PPP/CMUX and the previously running station have actually been stopped. */
    esp_err_t result = command_mode();
    if (result != ESP_OK) return result;
    if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_wifi_started) {
        result = esp_wifi_stop();
        if (result == ESP_OK) s_wifi_started = false;
    }
    xSemaphoreGive(s_wifi_mutex);
    if (result != ESP_OK) return result;
    xEventGroupClearBits(s_events, WIFI_IP_BIT | PPP_IP_BIT | WIFI_BUSY_BIT);
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_active = TRACKER_NET_NONE;
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

static void network_worker(void *argument)
{
    (void)argument;
    tracker_config_t config;
    for (;;) {
        EventBits_t bits = xEventGroupGetBits(s_events);
        if (bits & PAUSE_REQUEST_BIT) {
            if (!(bits & PAUSED_BIT)) {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                uint32_t generation = s_sleep_generation;
                xSemaphoreGive(s_state_mutex);
                esp_err_t result = shutdown_for_sleep();
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                s_sleep_result = result;
                s_sleep_ack_generation = generation;
                xSemaphoreGive(s_state_mutex);
                xEventGroupSetBits(s_events, PAUSED_BIT);
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }
        if (bits & PAUSED_BIT) {
            release_modem_holds();
            s_modem_ready = false;
            s_next_probe_ms = monotonic_ms() + 2000;
            s_next_gnss_ms = 0;
            s_next_lte_ms = 0;
            memset(&s_wifi_policy, 0, sizeof(s_wifi_policy));
            s_wifi_first_until_ms = monotonic_ms() + WIFI_FIRST_MS;
            xEventGroupClearBits(s_events, PAUSED_BIT);
        }
        bool setup = (xEventGroupGetBits(s_events) & SETUP_REQUEST_BIT) != 0;
        if (setup && !(xEventGroupGetBits(s_events) & SETUP_READY_BIT) &&
            (xEventGroupGetBits(s_events) & HTTP_IDLE_BIT)) {
            esp_err_t result = pause_uplinks_for_setup();
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_setup_result = result;
            xSemaphoreGive(s_state_mutex);
            if (xEventGroupGetBits(s_events) & SETUP_REQUEST_BIT) {
                xEventGroupSetBits(s_events, SETUP_READY_BIT);
            }
        }
        const EventBits_t config_bits = CONFIG_DIRTY_BIT | WIFI_CONFIG_DIRTY_BIT;
        if ((xEventGroupGetBits(s_events) & config_bits) &&
            xSemaphoreTake(s_http_mutex, 0) == pdTRUE) {
            /* DIRTY prevents a new publish; this lock and actual worker-idle
             * check also cover cleanup after a timed-out caller. */
            if (xEventGroupGetBits(s_events) & HTTP_IDLE_BIT) {
                xEventGroupSetBits(s_events, CONFIG_APPLYING_BIT);
                EventBits_t changes = xEventGroupClearBits(s_events, config_bits);
                if (changes & CONFIG_DIRTY_BIT) {
                    if (command_mode() != ESP_OK) set_error("Modem reconnect pending");
                    s_next_lte_ms = 0;
                }
                if ((changes & WIFI_CONFIG_DIRTY_BIT) && configure_wifi() != ESP_OK)
                    xEventGroupSetBits(s_events, WIFI_CONFIG_DIRTY_BIT);
                xEventGroupClearBits(s_events, CONFIG_APPLYING_BIT);
            }
            xSemaphoreGive(s_http_mutex);
        }
        get_config(&config);
        int64_t now = monotonic_ms();
        if (!setup || (xEventGroupGetBits(s_events) & SETUP_WIFI_ALLOWED_BIT)) maintain_wifi(&config, now);
        maintain_modem(&config, now);
        snapshot_gnss_runtime();
        if (!(xEventGroupGetBits(s_events) & PAUSE_REQUEST_BIT)) maintain_gnss(monotonic_ms());
        if (!(xEventGroupGetBits(s_events) & PAUSE_REQUEST_BIT) &&
            s_modem_ready && monotonic_ms() >= s_next_sim_ms) (void)query_sim();
        if (!setup && !(xEventGroupGetBits(s_events) & (PAUSE_REQUEST_BIT | SETUP_REQUEST_BIT))) {
            maintain_lte(&config, monotonic_ms());
            select_route(&config);
        } else if (xEventGroupGetBits(s_events) & SETUP_WIFI_ALLOWED_BIT) {
            select_route(&config);
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
    }
}

esp_err_t tracker_net_init(const tracker_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (s_initialized) return tracker_net_reconfigure(config);
    s_state_mutex = xSemaphoreCreateMutex();
    s_wifi_mutex = xSemaphoreCreateMutex();
    s_http_mutex = xSemaphoreCreateMutex();
    s_pin_mutex = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    if (!s_state_mutex || !s_wifi_mutex || !s_http_mutex || !s_pin_mutex || !s_events) return ESP_ERR_NO_MEM;
    s_config = *config;
    s_gnss_status.gps_satellites = s_gnss_status.glonass_satellites = s_gnss_status.beidou_satellites = -1;
    s_gnss_status.hdop = -1;
    esp_err_t result = esp_netif_init();
    if (result != ESP_OK) return result;
    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_config_t ppp_config = ESP_NETIF_DEFAULT_PPP();
    s_ppp = esp_netif_new(&ppp_config);
    if (!s_sta || !s_ap || !s_ppp) return ESP_ERR_NO_MEM;
    esp_netif_ip_info_t ap_ip = {0};
    IP4_ADDR(&ap_ip.ip, 192, 168, 4, 1);
    IP4_ADDR(&ap_ip.gw, 192, 168, 4, 1);
    IP4_ADDR(&ap_ip.netmask, 255, 255, 255, 0);
    result = esp_netif_dhcps_stop(s_ap);
    if (result != ESP_OK && result != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return result;
    result = esp_netif_set_ip_info(s_ap, &ap_ip);
    if (result != ESP_OK) return result;
    /* stop() leaves DHCP STOPPED, which AP_START will not restart. On this
     * still-down interface start() rearms INIT for automatic lease service. */
    result = esp_netif_dhcps_start(s_ap);
    if (result != ESP_OK) return result;
    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&wifi_config);
    if (result != ESP_OK) return result;
    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result != ESP_OK) return result;
    result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result != ESP_OK) return result;
    result = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event, NULL);
    if (result != ESP_OK) return result;
    result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    if (result != ESP_OK) return result;
    result = configure_wifi();
    if (result != ESP_OK) return result;
    esp_sntp_config_t clock_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    clock_config.start = false;
    s_sntp_initialized = esp_netif_sntp_init(&clock_config) == ESP_OK;
    gpio_config_t inputs = {
        .pin_bit_mask = 1ULL << MODEM_STATUS,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    result = gpio_config(&inputs);
    if (result != ESP_OK) return result;
    release_modem_holds();
    s_next_probe_ms = monotonic_ms() + 2000;
    s_initialized = true;
    xEventGroupSetBits(s_events, HTTP_IDLE_BIT);
    if (xTaskCreate(http_worker, "tracker_http", 8192, NULL, 3, &s_http_worker) != pdPASS) {
        s_initialized = false;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(network_worker, "tracker_net", 8192, NULL, 4, &s_worker) != pdPASS) {
        s_initialized = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Network worker started (Wi-Fi, GNSS, SIM7600 PPP/CMUX)");
    return ESP_OK;
}

static void close_http(void)
{
    if (s_http) {
        esp_http_client_cleanup(s_http);
        s_http = NULL;
    }
    s_http_network = TRACKER_NET_NONE;
}

/* Caller holds s_http_mutex. DNS resolution may block inside ESP-TLS: keep the
 * worker and its buffers alive after a caller deadline, and refuse sleep until
 * cleanup has really finished. No task deletion or concurrent client cleanup. */
static esp_err_t cancel_http_and_wait(int64_t deadline)
{
    if (!(xEventGroupGetBits(s_events) & HTTP_IDLE_BIT)) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_http_job.cancel = true;
        xSemaphoreGive(s_state_mutex);
        uint32_t remaining = tracker_net_remaining_ms(monotonic_ms(), deadline);
        EventBits_t bits = xEventGroupWaitBits(s_events, HTTP_IDLE_BIT, pdFALSE, pdTRUE,
                                             pdMS_TO_TICKS(remaining));
        if (!(bits & HTTP_IDLE_BIT)) return ESP_ERR_TIMEOUT;
    }
    close_http();
    return ESP_OK;
}

static bool http_cancelled(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool cancelled = s_http_job.cancel;
    xSemaphoreGive(s_state_mutex);
    return tracker_net_request_expired(monotonic_ms(), s_http_job.deadline_ms, cancelled);
}

static esp_err_t output_http_event(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0)
        tracker_output_response_append(event->data, (size_t)event->data_len);
    return ESP_OK;
}

static void http_worker(void *argument)
{
    (void)argument;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        tracker_config_t *config = &s_http_job.config;
        esp_err_t result = ESP_ERR_TIMEOUT;
        int status = 0;
        tracker_output_response_begin();
        if (s_http && s_http_network != s_http_job.network) close_http();
        if (!s_http && !http_cancelled()) {
            esp_http_client_config_t http_config = {
                .url = config->endpoint,
                .method = HTTP_METHOD_POST,
                .timeout_ms = HTTP_TIMEOUT_MS,
                .is_async = true,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .disable_auto_redirect = true,
                .keep_alive_enable = true,
                .buffer_size = 1024,
                .buffer_size_tx = 2048,
                .user_agent = "CarTracker/1",
                .event_handler = output_http_event,
            };
            s_http = esp_http_client_init(&http_config);
            s_http_network = s_http_job.network;
        }
        if (s_http && !http_cancelled()) {
            result = esp_http_client_set_header(s_http, "Content-Type", "application/x-www-form-urlencoded");
            if (result == ESP_OK && config->access_client_id[0]) {
                result = esp_http_client_set_header(s_http, "CF-Access-Client-Id", config->access_client_id);
                if (result == ESP_OK) result = esp_http_client_set_header(s_http, "CF-Access-Client-Secret", config->access_client_secret);
            }
            if (result == ESP_OK) result = esp_http_client_set_post_field(s_http, s_http_job.form, s_http_job.form_length);
            if (result == ESP_OK) {
                do {
                    if (http_cancelled()) {
                        result = ESP_ERR_TIMEOUT;
                        break;
                    }
                    int remaining = (int)tracker_net_remaining_ms(monotonic_ms(), s_http_job.deadline_ms);
                    esp_http_client_set_timeout_ms(s_http, remaining > 0 ? remaining : 1);
                    result = esp_http_client_perform(s_http);
                    if (result == ESP_ERR_HTTP_EAGAIN) vTaskDelay(pdMS_TO_TICKS(10));
                } while (result == ESP_ERR_HTTP_EAGAIN);
                if (http_cancelled()) result = ESP_ERR_TIMEOUT;
                if (result == ESP_OK) status = esp_http_client_get_status_code(s_http);
            }
        } else if (!s_http && !http_cancelled()) {
            result = ESP_ERR_NO_MEM;
        }
        tracker_output_response_finish(result == ESP_OK && status == 200 && !http_cancelled());
        if (result != ESP_OK || status < 200 || status >= 300) {
            close_http();
            if (result != ESP_OK) {
                set_error("HTTPS transport failed or deadline expired");
                if (s_http_job.network == TRACKER_NET_WIFI && config->network_mode == TRACKER_AUTO) {
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    s_wifi_bad_until_ms = monotonic_ms() + 60000;
                    xSemaphoreGive(s_state_mutex);
                }
            } else {
                char message[64];
                snprintf(message, sizeof(message), "HTTPS endpoint returned %d", status);
                set_error(message);
                result = ESP_FAIL;
            }
        } else {
            esp_http_client_set_post_field(s_http, NULL, 0);
            set_error("");
        }
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_http_job.result = result;
        s_http_job.status = status;
        xSemaphoreGive(s_state_mutex);
        xEventGroupSetBits(s_events, HTTP_IDLE_BIT);
    }
}

static bool wifi_credentials_equal(const tracker_config_t *left, const tracker_config_t *right)
{
    return strncmp(left->wifi_ssid, right->wifi_ssid, sizeof(left->wifi_ssid)) == 0 &&
           strncmp(left->wifi_password, right->wifi_password, sizeof(left->wifi_password)) == 0 &&
           strncmp(left->wifi_secondary_ssid, right->wifi_secondary_ssid, sizeof(left->wifi_secondary_ssid)) == 0 &&
           strncmp(left->wifi_secondary_password, right->wifi_secondary_password, sizeof(left->wifi_secondary_password)) == 0;
}

esp_err_t tracker_net_begin_config_update(bool *previous_pin_pause)
{
    if (!s_initialized || !previous_pin_pause) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_http_mutex, pdMS_TO_TICKS(16000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t result = cancel_http_and_wait(monotonic_ms() + 16000);
    if (result == ESP_OK && xSemaphoreTake(s_pin_mutex, pdMS_TO_TICKS(3000)) != pdTRUE)
        result = ESP_ERR_TIMEOUT;
    if (result == ESP_OK) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        *previous_pin_pause = s_pin_config_paused;
        s_pin_config_paused = true;
        xSemaphoreGive(s_state_mutex);
        /* PIN stays locked until the caller commits or abandons the save. */
    }
    xSemaphoreGive(s_http_mutex);
    return result;
}

void tracker_net_end_config_update(bool committed, bool previous_pin_pause)
{
    if (!committed) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_pin_config_paused = previous_pin_pause;
        xSemaphoreGive(s_state_mutex);
    }
    xSemaphoreGive(s_pin_mutex);
}

esp_err_t tracker_net_reconfigure(const tracker_config_t *config)
{
    return tracker_net_reconfigure_with_pin_retry(config, false);
}

esp_err_t tracker_net_reconfigure_with_pin_retry(const tracker_config_t *config, bool retry_pin)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_http_mutex, pdMS_TO_TICKS(16000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t result = cancel_http_and_wait(monotonic_ms() + 16000);
    if (result != ESP_OK) {
        xSemaphoreGive(s_http_mutex);
        return result;
    }
    if (xSemaphoreTake(s_pin_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        xSemaphoreGive(s_http_mutex);
        return ESP_ERR_TIMEOUT;
    }
    if (retry_pin) {
        /* A persistence error may follow a partially completed write. Keep
         * unlock paused on that path, even if the durable flag was cleared. */
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_pin_config_paused = true;
        xSemaphoreGive(s_state_mutex);
        result = tracker_store_set_sim_pin_blocked(false);
        if (result != ESP_OK) {
            xSemaphoreGive(s_pin_mutex);
            xSemaphoreGive(s_http_mutex);
            return result;
        }
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool wifi_changed = !wifi_credentials_equal(&s_config, config) ||
        ((s_config.network_mode == TRACKER_CELLULAR_ONLY) !=
         (config->network_mode == TRACKER_CELLULAR_ONLY));
    s_config = *config;
    s_wifi_bad_until_ms = 0;
    s_pin_config_paused = false;
    xSemaphoreGive(s_state_mutex);
    xSemaphoreGive(s_pin_mutex);
    xEventGroupSetBits(s_events, CONFIG_DIRTY_BIT | (wifi_changed ? WIFI_CONFIG_DIRTY_BIT : 0));
    xSemaphoreGive(s_http_mutex);
    xTaskNotifyGive(s_worker);
    return ESP_OK;
}

esp_err_t tracker_net_apply_wifi(const tracker_config_t *config)
{
    if (!tracker_config_validate_wifi(config, NULL, 0)) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool changed = !wifi_credentials_equal(&s_config, config);
    memcpy(s_config.wifi_ssid, config->wifi_ssid, sizeof(s_config.wifi_ssid));
    memcpy(s_config.wifi_password, config->wifi_password, sizeof(s_config.wifi_password));
    memcpy(s_config.wifi_secondary_ssid, config->wifi_secondary_ssid, sizeof(s_config.wifi_secondary_ssid));
    memcpy(s_config.wifi_secondary_password, config->wifi_secondary_password, sizeof(s_config.wifi_secondary_password));
    s_wifi_bad_until_ms = 0;
    xSemaphoreGive(s_state_mutex);
    if (changed) {
        xEventGroupSetBits(s_events, WIFI_CONFIG_DIRTY_BIT);
        xTaskNotifyGive(s_worker);
    }
    return ESP_OK;
}

esp_err_t tracker_net_pause_pin(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_pin_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_pin_config_paused = true;
    xSemaphoreGive(s_state_mutex);
    xSemaphoreGive(s_pin_mutex);
    return ESP_OK;
}

void tracker_net_get_wifi_status(tracker_wifi_status_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    if (!s_initialized) return;
    EventBits_t bits = xEventGroupGetBits(s_events);
    wifi_ap_record_t access_point;
    if (esp_wifi_sta_get_ap_info(&access_point) == ESP_OK) {
        status->connected = true;
        memcpy(status->ssid, access_point.ssid, sizeof(status->ssid) - 1);
        status->rssi = access_point.rssi;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (strcmp(status->ssid, s_config.wifi_ssid) == 0) status->profile = 1;
        else if (strcmp(status->ssid, s_config.wifi_secondary_ssid) == 0) status->profile = 2;
        xSemaphoreGive(s_state_mutex);
        esp_netif_ip_info_t ip;
        if ((bits & WIFI_IP_BIT) && esp_netif_get_ip_info(s_sta, &ip) == ESP_OK && ip.ip.addr) {
            status->has_ip = true;
            snprintf(status->ip, sizeof(status->ip), IPSTR, IP2STR(&ip.ip));
        }
    } else if (bits & WIFI_BUSY_BIT) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        status->profile = s_wifi_target_profile;
        memcpy(status->ssid, s_wifi_target_ssid, sizeof(status->ssid));
        xSemaphoreGive(s_state_mutex);
    }
    status->connecting = !status->has_ip && (status->connected || (bits & WIFI_BUSY_BIT));
}

esp_err_t tracker_net_resume(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    ++s_sleep_generation;
    s_gnss_requested = true;
    xEventGroupClearBits(s_events, PAUSE_REQUEST_BIT);
    xSemaphoreGive(s_state_mutex);
    xTaskNotifyGive(s_worker);
    return ESP_OK;
}

esp_err_t tracker_net_set_gnss_enabled(bool enabled)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_gnss_requested = enabled;
    if (!enabled) {
        memset(&s_fix, 0, sizeof(s_fix));
        s_fix_received_ms = 0;
    }
    xSemaphoreGive(s_state_mutex);
    xTaskNotifyGive(s_worker);
    return ESP_OK;
}

esp_err_t tracker_net_prepare_sleep(uint32_t timeout_ms)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (tracker_net_setup_active() || (xEventGroupGetBits(s_events) & SETUP_REQUEST_BIT)) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    uint32_t generation = ++s_sleep_generation;
    xSemaphoreGive(s_state_mutex);
    int64_t deadline = monotonic_ms() + timeout_ms;
    if (xSemaphoreTake(s_http_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t closed = cancel_http_and_wait(deadline);
    xSemaphoreGive(s_http_mutex);
    if (closed != ESP_OK) return closed;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (generation != s_sleep_generation) {
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupSetBits(s_events, PAUSE_REQUEST_BIT);
    xSemaphoreGive(s_state_mutex);
    xTaskNotifyGive(s_worker);
    uint32_t remaining = tracker_net_remaining_ms(monotonic_ms(), deadline);
    EventBits_t bits = xEventGroupWaitBits(s_events, PAUSED_BIT, pdFALSE, pdTRUE,
                                         pdMS_TO_TICKS(remaining));
    if (!(bits & PAUSED_BIT)) return ESP_ERR_TIMEOUT;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    esp_err_t result = tracker_net_sleep_ack_valid(generation, s_sleep_generation, s_sleep_ack_generation,
                                                  (xEventGroupGetBits(s_events) & PAUSE_REQUEST_BIT) != 0)
                           ? s_sleep_result : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_state_mutex);
    return result;
}

esp_err_t tracker_net_read_fix(tracker_fix_t *fix)
{
    if (!fix) return ESP_ERR_INVALID_ARG;
    memset(fix, 0, sizeof(*fix));
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool valid = s_fix.valid && monotonic_ms() - s_fix_received_ms <= GNSS_FRESH_MS;
    if (valid) *fix = s_fix;
    xSemaphoreGive(s_state_mutex);
    return valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void tracker_net_get_gnss_status(tracker_gnss_status_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    status->last_reply_age_ms = status->detail_age_ms = status->power_age_ms = status->search_elapsed_ms = -1;
    status->gps_satellites = status->glonass_satellites = status->beidou_satellites = -1;
    status->hdop = -1;
    snprintf(status->state, sizeof(status->state), "starting");
    if (!s_initialized) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *status = s_gnss_status;
    int64_t now = monotonic_ms();
    status->requested = s_gnss_requested;
    status->last_reply_age_ms = s_gnss_last_reply_ms ? now - s_gnss_last_reply_ms : -1;
    status->detail_age_ms = s_gnss_detail_received_ms ? now - s_gnss_detail_received_ms : -1;
    status->power_age_ms = s_gnss_power_received_ms ? now - s_gnss_power_received_ms : -1;
    bool current_fix = s_fix.valid && now - s_fix_received_ms <= GNSS_FRESH_MS;
    status->search_elapsed_ms = status->requested && status->receiver_running && !current_fix && s_gnss_search_started_ms
        ? now - s_gnss_search_started_ms : -1;
    xSemaphoreGive(s_state_mutex);
    if (!status->modem_ready) {
        status->receiver_state_known = false;
        status->antenna_state_known = false;
    }
    if (!status->details_available || status->detail_age_ms < 0 || status->detail_age_ms > GNSS_DETAIL_FRESH_MS ||
        !status->requested || !status->receiver_running) {
        status->details_available = false;
        status->fix_mode = 0;
        status->gps_satellites = status->glonass_satellites = status->beidou_satellites = -1;
        status->hdop = -1;
    }
    const char *state;
    if (!status->requested) state = "off";
    else if (status->error[0]) state = "error";
    else if (!status->modem_ready || !status->receiver_running) state = "starting";
    else if (status->last_reply_age_ms > GNSS_FRESH_MS) {
        state = "error";
        snprintf(status->error, sizeof(status->error), "GNSS position response is stale");
    } else if (current_fix) state = "fix";
    else if (status->last_reply_age_ms >= 0) state = "searching";
    else state = "starting";
    snprintf(status->state, sizeof(status->state), "%s", state);
}

tracker_network_t tracker_net_active(void)
{
    if (!s_initialized) return TRACKER_NET_NONE;
    EventBits_t bits = xEventGroupGetBits(s_events);
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    tracker_network_t result = s_active;
    xSemaphoreGive(s_state_mutex);
    if ((result == TRACKER_NET_WIFI && !(bits & WIFI_IP_BIT)) ||
        (result == TRACKER_NET_LTE && !(bits & PPP_IP_BIT))) return TRACKER_NET_NONE;
    return result;
}

void tracker_net_get_error(char *buffer, size_t size)
{
    if (!buffer || !size) return;
    buffer[0] = '\0';
    if (!s_initialized) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    snprintf(buffer, size, "%s", s_error);
    xSemaphoreGive(s_state_mutex);
}

void tracker_net_get_sim_status(char *buffer, size_t size)
{
    if (!buffer || !size) return;
    snprintf(buffer, size, "unknown");
    if (!s_initialized) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    snprintf(buffer, size, "%s", s_sim_status);
    xSemaphoreGive(s_state_mutex);
}

bool tracker_net_config_applied(void)
{
    return s_initialized && !(xEventGroupGetBits(s_events) &
        (CONFIG_DIRTY_BIT | WIFI_CONFIG_DIRTY_BIT | CONFIG_APPLYING_BIT));
}

static bool publish_allowed(tracker_network_t network)
{
    EventBits_t bits = xEventGroupGetBits(s_events);
    return network != TRACKER_NET_NONE && tracker_net_config_applied() && tracker_net_setup_publish_allowed(
        network == TRACKER_NET_WIFI, (bits & PAUSE_REQUEST_BIT) != 0,
        (bits & SETUP_REQUEST_BIT) != 0, (bits & AP_ACTIVE_BIT) != 0,
        (bits & SETUP_READY_BIT) != 0, (bits & SETUP_WIFI_ALLOWED_BIT) != 0);
}

esp_err_t tracker_net_publish(const tracker_sample_t *sample, int *http_status)
{
    int64_t deadline = monotonic_ms() + HTTP_TIMEOUT_MS;
    if (http_status) *http_status = 0;
    if (!sample || !s_initialized) return ESP_ERR_INVALID_ARG;
    tracker_network_t network = tracker_net_active();
    if (!publish_allowed(network)) return ESP_ERR_INVALID_STATE;
    if (time(NULL) < EARLIEST_VALID_TIME) {
        set_error("Waiting for GNSS/NTP time before TLS");
        return ESP_ERR_INVALID_STATE;
    }
    tracker_config_t config;
    get_config(&config);
    if (strncmp(config.endpoint, "https://", 8) != 0) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_http_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!publish_allowed(network) || !(xEventGroupGetBits(s_events) & HTTP_IDLE_BIT)) {
        xSemaphoreGive(s_http_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    int length = tracker_osmand_form(&config, sample, s_http_job.form, sizeof(s_http_job.form));
    if (length >= 0) {
        tracker_output_status_t output;
        tracker_output_get_status(&output);
        struct timeval now;
        gettimeofday(&now, NULL);
        int extra = snprintf(s_http_job.form + length, sizeof(s_http_job.form) - (size_t)length,
            "&output=%s&outputReadback=%s&outputAvailable=%s&outputGpio=%d&outputReportedAt=%lld&outputResult=%s",
            output.enabled ? "true" : "false", output.readback ? "true" : "false",
            output.available ? "true" : "false", TRACKER_OUTPUT_GPIO,
            (long long)now.tv_sec * 1000 + now.tv_usec / 1000, output.result);
        if (extra < 0 || (size_t)extra >= sizeof(s_http_job.form) - (size_t)length) length = -1;
        else length += extra;
    }
    if (length < 0) {
        xSemaphoreGive(s_http_mutex);
        return ESP_ERR_INVALID_SIZE;
    }
    s_http_job.config = config;
    s_http_job.form_length = length;
    s_http_job.network = network;
    s_http_job.deadline_ms = deadline;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_http_job.cancel = false;
    s_http_job.result = ESP_ERR_TIMEOUT;
    s_http_job.status = 0;
    xSemaphoreGive(s_state_mutex);
    xEventGroupClearBits(s_events, HTTP_IDLE_BIT);
    xTaskNotifyGive(s_http_worker);
    uint32_t remaining = tracker_net_remaining_ms(monotonic_ms(), deadline);
    EventBits_t bits = xEventGroupWaitBits(s_events, HTTP_IDLE_BIT, pdFALSE, pdTRUE,
                                         pdMS_TO_TICKS(remaining));
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    esp_err_t result;
    if (bits & HTTP_IDLE_BIT) {
        result = s_http_job.result;
        if (http_status) *http_status = s_http_job.status;
    } else {
        s_http_job.cancel = true;
        result = ESP_ERR_TIMEOUT;
    }
    xSemaphoreGive(s_state_mutex);
    xSemaphoreGive(s_http_mutex);
    return result;
}

esp_err_t tracker_net_setup_start(const char *ssid, const char *password)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!ssid || !password || strlen(ssid) == 0 || strlen(ssid) > 32 ||
        strlen(password) < 8 || strlen(password) > 63) return ESP_ERR_INVALID_ARG;
    if (tracker_net_setup_active()) return ESP_ERR_INVALID_STATE;
    int64_t deadline = monotonic_ms() + 50000;
    tracker_net_resume();
    if (xSemaphoreTake(s_pin_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_pin_config_paused = true;
    xSemaphoreGive(s_state_mutex);
    xSemaphoreGive(s_pin_mutex);
    /* Block new requests before canceling HTTPS, then ask the UART owner to
     * stop PPP. No AP or HTTP listener exists during this transition. */
    xEventGroupClearBits(s_events, SETUP_READY_BIT | SETUP_WIFI_ALLOWED_BIT);
    xEventGroupSetBits(s_events, SETUP_REQUEST_BIT);
    esp_err_t result = ESP_ERR_TIMEOUT;
    uint32_t remaining = tracker_net_remaining_ms(monotonic_ms(), deadline);
    if (xSemaphoreTake(s_http_mutex, pdMS_TO_TICKS(remaining)) != pdTRUE) goto failed;
    result = cancel_http_and_wait(deadline);
    xSemaphoreGive(s_http_mutex);
    if (result != ESP_OK) goto failed;
    xTaskNotifyGive(s_worker);
    remaining = tracker_net_remaining_ms(monotonic_ms(), deadline);
    EventBits_t bits = xEventGroupWaitBits(s_events, SETUP_READY_BIT, pdFALSE, pdTRUE,
                                         pdMS_TO_TICKS(remaining));
    if (!(bits & SETUP_READY_BIT)) { result = ESP_ERR_TIMEOUT; goto failed; }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    result = s_setup_result;
    xSemaphoreGive(s_state_mutex);
    if (result != ESP_OK) goto failed;
    if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        result = ESP_ERR_TIMEOUT;
        goto failed;
    }
    wifi_config_t access_point = {0};
    memcpy(access_point.ap.ssid, ssid, strlen(ssid));
    access_point.ap.ssid_len = (uint8_t)strlen(ssid);
    memcpy(access_point.ap.password, password, strlen(password));
    access_point.ap.channel = 1;
    access_point.ap.authmode = WIFI_AUTH_WPA2_PSK;
    access_point.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
    access_point.ap.max_connection = 1;
    access_point.ap.pmf_cfg.capable = true;
    access_point.ap.pmf_cfg.required = false;
    result = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (result == ESP_OK) result = esp_wifi_set_config(WIFI_IF_AP, &access_point);
    if (result == ESP_OK) result = ensure_wifi_started();
    if (result == ESP_OK) xEventGroupSetBits(s_events, AP_ACTIVE_BIT);
    xSemaphoreGive(s_wifi_mutex);
    if (result != ESP_OK) goto failed;
    return result;

failed:
    /* No setup listener is allowed on failure. Restore tracking eligibility. */
    if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
        xSemaphoreGive(s_wifi_mutex);
    }
    xEventGroupClearBits(s_events, SETUP_REQUEST_BIT | SETUP_READY_BIT | SETUP_WIFI_ALLOWED_BIT);
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_pin_config_paused = false;
    xSemaphoreGive(s_state_mutex);
    xTaskNotifyGive(s_worker);
    return result;
}

esp_err_t tracker_net_setup_allow_wifi(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    const EventBits_t required = AP_ACTIVE_BIT | SETUP_REQUEST_BIT | SETUP_READY_BIT;
    if ((xEventGroupGetBits(s_events) & required) != required) return ESP_ERR_INVALID_STATE;
    /* The caller has already verified its AP-only listening sockets. There
     * is no cellular/PIN permission change here, including CELLULAR_ONLY mode. */
    xEventGroupSetBits(s_events, SETUP_WIFI_ALLOWED_BIT | WIFI_CONFIG_DIRTY_BIT);
    xTaskNotifyGive(s_worker);
    return ESP_OK;
}

esp_err_t tracker_net_setup_stop(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result == ESP_OK) {
        xEventGroupClearBits(s_events, AP_ACTIVE_BIT | SETUP_REQUEST_BIT | SETUP_READY_BIT | SETUP_WIFI_ALLOWED_BIT);
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        bool cellular_only = s_config.network_mode == TRACKER_CELLULAR_ONLY;
        xSemaphoreGive(s_state_mutex);
        /* APSTA -> STA keeps its association and lease. A successful Wi-Fi
         * test only has to stop when the saved mode explicitly forbids STA. */
        if (cellular_only) xEventGroupSetBits(s_events, WIFI_CONFIG_DIRTY_BIT);
    }
    xSemaphoreGive(s_wifi_mutex);
    if (result == ESP_OK) xTaskNotifyGive(s_worker);
    return result;
}

bool tracker_net_setup_active(void)
{
    return s_initialized && (xEventGroupGetBits(s_events) & AP_ACTIVE_BIT) != 0;
}

esp_err_t tracker_net_scan(wifi_ap_record_t *records, uint16_t *count)
{
    if (!records || !count || !*count) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    /* The same scan serves AP setup and the authorized station-side web UI.
     * Never restart a stopped radio or race a requested sleep transition. */
    if (!s_wifi_started ||
        (xEventGroupGetBits(s_events) & (WIFI_BUSY_BIT | PAUSE_REQUEST_BIT | PAUSED_BIT))) {
        xSemaphoreGive(s_wifi_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupSetBits(s_events, SCAN_BUSY_BIT);
    wifi_scan_config_t scan = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 60, .max = 120},
    };
    esp_err_t result = esp_wifi_scan_start(&scan, true);
    if (result == ESP_OK) result = esp_wifi_scan_get_ap_records(count, records);
    if (result != ESP_OK) *count = 0;
    xEventGroupClearBits(s_events, SCAN_BUSY_BIT);
    xSemaphoreGive(s_wifi_mutex);
    return result;
}
