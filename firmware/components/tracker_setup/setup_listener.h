#pragma once
#ifdef ESP_PLATFORM
#include "esp_http_server.h"
#else
#include "test/setup_listener_test_platform.h"
#endif

/* Bind the listening socket to SoftAP before listen(), and the HTTPD control
 * socket to loopback. A checked link wrapper avoids a private HTTPD fork. */
esp_err_t tracker_httpd_start_ap_only(httpd_handle_t *server, const httpd_config_t *config);
/* Only WIFI_AP_DEF / WIFI_STA_DEF are admitted. Address is network-order. */
esp_err_t tracker_httpd_start_wifi(httpd_handle_t *server, const httpd_config_t *config,
                                  const char *interface_key, uint32_t address);
