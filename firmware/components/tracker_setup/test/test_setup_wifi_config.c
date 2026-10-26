#include "setup_wifi_config.h"
#include "tracker_core.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    tracker_config_t saved;
    tracker_config_defaults(&saved);
    strcpy(saved.wifi_ssid, "Home");
    strcpy(saved.wifi_password, "home-password");
    strcpy(saved.wifi_secondary_ssid, "Hotspot");
    strcpy(saved.wifi_secondary_password, "hotspot-password");
    strcpy(saved.access_client_secret, "must-not-cross-the-wifi-handoff");
    strcpy(saved.sim_pin, "1234");
    tracker_config_t pending = {0}, request = {0};
    tracker_setup_copy_wifi_config(&pending, &saved);
    tracker_setup_copy_wifi_config(&request, &pending);
    char error[128];
    /* This is the validation used by the asynchronous network API. */
    assert(tracker_config_validate_wifi(&request, error, sizeof(error)));
    assert(!strcmp(request.wifi_ssid, "Home"));
    assert(!strcmp(request.wifi_password, "home-password"));
    assert(!strcmp(request.wifi_secondary_ssid, "Hotspot"));
    assert(!strcmp(request.wifi_secondary_password, "hotspot-password"));
    assert(!request.access_client_secret[0]);
    assert(!request.sim_pin[0]);
    assert(!request.endpoint[0]);

    /* Updating the running setup copy must preserve unrelated settings. */
    strcpy(saved.endpoint, "https://tracker.example.test/ingest");
    strcpy(request.wifi_secondary_ssid, "Other hotspot");
    tracker_setup_copy_wifi_config(&saved, &request);
    assert(!strcmp(saved.endpoint, "https://tracker.example.test/ingest"));
    assert(!strcmp(saved.access_client_secret, "must-not-cross-the-wifi-handoff"));
    assert(!strcmp(saved.sim_pin, "1234"));
    assert(!strcmp(saved.wifi_secondary_ssid, "Other hotspot"));
    puts("setup Wi-Fi config: deferred apply validation and field isolation passed");
    return 0;
}
