#include "setup_listener.h"
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#endif

/* HTTPD 5.5 creates its listener and control socket synchronously in start().
 * Only binds by that calling task, on the two configured ports, are affected.
 * Normal client sockets, SNTP, DNS, and HTTPD's unbound sender pass through. */
static atomic_flag s_start_lock = ATOMIC_FLAG_INIT;
static _Atomic(TaskHandle_t) s_owner;
static struct ifreq s_ap;
static uint32_t s_address;
static uint16_t s_http_port, s_control_port;
static bool s_http_bound, s_control_bound;

int __real_lwip_bind(int socket_fd, const struct sockaddr *address, socklen_t length);

int __wrap_lwip_bind(int socket_fd, const struct sockaddr *address, socklen_t length)
{
    TaskHandle_t owner = atomic_load_explicit(&s_owner, memory_order_acquire);
    if (!owner || owner != xTaskGetCurrentTaskHandle() || !address)
        return __real_lwip_bind(socket_fd, address, length);
    uint16_t port;
    if (address->sa_family == AF_INET && length >= sizeof(struct sockaddr_in))
        port = ntohs(((const struct sockaddr_in *)address)->sin_port);
    else if (address->sa_family == AF_INET6 && length >= sizeof(struct sockaddr_in6))
        port = ntohs(((const struct sockaddr_in6 *)address)->sin6_port);
    else return __real_lwip_bind(socket_fd, address, length);
    if (port != s_http_port && port != s_control_port)
        return __real_lwip_bind(socket_fd, address, length);
    int type = 0;
    socklen_t type_length = sizeof(type);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_TYPE, &type, &type_length) != 0) return -1;
    bool listener = port == s_http_port && type == SOCK_STREAM;
    bool control = port == s_control_port && type == SOCK_DGRAM;
    if (!listener && !control) { errno = EACCES; return -1; }
    if (listener && setsockopt(socket_fd, SOL_SOCKET, SO_BINDTODEVICE, &s_ap, sizeof(s_ap)) != 0)
        return -1;
    uint32_t local_ip = listener ? s_address : htonl(0x7f000001u);
    int result;
    if (address->sa_family == AF_INET) {
        struct sockaddr_in local = *(const struct sockaddr_in *)address;
        local.sin_addr.s_addr = local_ip;
        result = __real_lwip_bind(socket_fd, (const struct sockaddr *)&local, sizeof(local));
    } else {
        struct sockaddr_in6 local = *(const struct sockaddr_in6 *)address;
        unsigned char *bytes = (unsigned char *)&local.sin6_addr;
        memset(bytes, 0, sizeof(local.sin6_addr));
        bytes[10] = bytes[11] = 0xff;
        memcpy(bytes + 12, &local_ip, sizeof(local_ip));
        result = __real_lwip_bind(socket_fd, (const struct sockaddr *)&local, sizeof(local));
    }
    if (result == 0) {
        if (listener) s_http_bound = true;
        if (control) s_control_bound = true;
    }
    return result;
}

esp_err_t tracker_httpd_start_wifi(httpd_handle_t *server, const httpd_config_t *config,
                                  const char *interface_key, uint32_t address)
{
    if (!server || !config || !config->server_port || !config->ctrl_port || !address ||
        !interface_key || (strcmp(interface_key, "WIFI_AP_DEF") && strcmp(interface_key, "WIFI_STA_DEF")))
        return ESP_ERR_INVALID_ARG;
    *server = NULL;
    if (atomic_flag_test_and_set_explicit(&s_start_lock, memory_order_acquire))
        return ESP_ERR_INVALID_STATE;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey(interface_key);
    int64_t deadline = esp_timer_get_time() + 3000000;
    while (ap && !esp_netif_is_netif_up(ap) && esp_timer_get_time() < deadline)
        vTaskDelay(pdMS_TO_TICKS(20));
    memset(&s_ap, 0, sizeof(s_ap));
    esp_netif_ip_info_t ip;
    if (!ap || !esp_netif_is_netif_up(ap) ||
        esp_netif_get_ip_info(ap, &ip) != ESP_OK || ip.ip.addr != address ||
        esp_netif_get_netif_impl_name(ap, s_ap.ifr_name) != ESP_OK || !s_ap.ifr_name[0])
        goto finished;
    s_http_port = config->server_port;
    s_control_port = config->ctrl_port;
    s_address = address;
    s_http_bound = s_control_bound = false;
    atomic_store_explicit(&s_owner, xTaskGetCurrentTaskHandle(), memory_order_release);
    result = httpd_start(server, config);
    atomic_store_explicit(&s_owner, NULL, memory_order_release);
    /* Fail closed if a future HTTPD version changes its initialization path or
     * if the linker wrapper is omitted. Never allow STA on an unbound server. */
    if (result == ESP_OK && (!s_http_bound || !s_control_bound)) {
        httpd_stop(*server);
        *server = NULL;
        result = ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK) ESP_LOGI("tracker_setup", "HTTP listener bound to %s; control socket on loopback", interface_key);
finished:
    atomic_flag_clear_explicit(&s_start_lock, memory_order_release);
    return result;
}

esp_err_t tracker_httpd_start_ap_only(httpd_handle_t *server, const httpd_config_t *config)
{
    return tracker_httpd_start_wifi(server, config, "WIFI_AP_DEF", htonl(0xc0a80401u));
}
