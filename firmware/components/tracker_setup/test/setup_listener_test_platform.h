#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_LOGI(...) ((void)0)
typedef void *TaskHandle_t;
typedef void *httpd_handle_t;
typedef struct { uint16_t server_port, ctrl_port; } httpd_config_t;
typedef struct { int unused; } esp_netif_t;
typedef struct { struct { uint32_t addr; } ip; } esp_netif_ip_info_t;
typedef unsigned socklen_t;
#define AF_INET 2
#define AF_INET6 10
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOL_SOCKET 1
#define SO_TYPE 3
#define SO_BINDTODEVICE 25
struct in_addr { uint32_t s_addr; };
struct in6_addr { unsigned char bytes[16]; };
struct sockaddr { unsigned char sa_len, sa_family; char data[14]; };
struct sockaddr_in { unsigned char sin_len, sin_family; uint16_t sin_port; struct in_addr sin_addr; char pad[8]; };
struct sockaddr_in6 { unsigned char sin6_len, sin6_family; uint16_t sin6_port; uint32_t flow; struct in6_addr sin6_addr; uint32_t scope; };
struct ifreq { char ifr_name[16]; };
static inline uint16_t htons(uint16_t value) { unsigned char bytes[2] = {(unsigned char)(value >> 8), (unsigned char)value}; uint16_t v; memcpy(&v, bytes, 2); return v; }
static inline uint16_t ntohs(uint16_t value) { const unsigned char *p = (const unsigned char *)&value; return (uint16_t)(((unsigned)p[0] << 8) | p[1]); }
static inline uint32_t htonl(uint32_t value) { unsigned char bytes[4] = {(unsigned char)(value >> 24), (unsigned char)(value >> 16), (unsigned char)(value >> 8), (unsigned char)value}; uint32_t v; memcpy(&v, bytes, 4); return v; }
#define pdMS_TO_TICKS(ms) (ms)
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskDelay(unsigned ticks);
int64_t esp_timer_get_time(void);
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key);
bool esp_netif_is_netif_up(esp_netif_t *netif);
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *ip);
esp_err_t esp_netif_get_netif_impl_name(esp_netif_t *netif, char *name);
int getsockopt(int fd, int level, int option, void *value, socklen_t *length);
int setsockopt(int fd, int level, int option, const void *value, socklen_t length);
esp_err_t httpd_start(httpd_handle_t *server, const httpd_config_t *config);
esp_err_t httpd_stop(httpd_handle_t server);
int __wrap_lwip_bind(int fd, const struct sockaddr *address, socklen_t length);
int __real_lwip_bind(int fd, const struct sockaddr *address, socklen_t length);
