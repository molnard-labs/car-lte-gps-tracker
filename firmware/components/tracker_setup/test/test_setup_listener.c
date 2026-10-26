#include "setup_listener.h"
#include <assert.h>
#include <stdio.h>
static esp_netif_t ap, sta;
static esp_netif_t *selected;
static int owner_id, other_id, server_id;
static TaskHandle_t current;
static bool up, bad_ip, bind_failure, missing_hook, ipv6, fail_control, missing_interface, wrong_socket_type;
static unsigned binding_calls, stopped, http_started, ap_bound_before_bind;
static int64_t now_us;
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current; }
void vTaskDelay(unsigned ticks) { now_us += (int64_t)ticks * 1000; }
int64_t esp_timer_get_time(void) { return now_us; }
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) {
    assert(!strcmp(key,"WIFI_AP_DEF") || !strcmp(key,"WIFI_STA_DEF"));
    selected=!strcmp(key,"WIFI_AP_DEF")?&ap:&sta; return missing_interface?NULL:selected;
}
bool esp_netif_is_netif_up(esp_netif_t *n) { assert(n==selected); return up; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *ip) {
    assert(n==selected); ip->ip.addr=htonl(bad_ip?0xc0a80101u:n==&ap?0xc0a80401u:0xc0a8322au); return ESP_OK;
}
esp_err_t esp_netif_get_netif_impl_name(esp_netif_t *n, char *name) { assert(n==selected); strcpy(name,n==&ap?"ap1":"st1"); return ESP_OK; }
int getsockopt(int fd,int level,int option,void *value,socklen_t *length) { assert(level==SOL_SOCKET&&option==SO_TYPE&&*length==sizeof(int)); *(int*)value=(fd==2||wrong_socket_type)?SOCK_DGRAM:SOCK_STREAM; return 0; }
int setsockopt(int fd,int level,int option,const void *value,socklen_t length) {
    assert(fd==1 && level==SOL_SOCKET && option==SO_BINDTODEVICE && length==sizeof(struct ifreq));
    assert(!strcmp(((const struct ifreq*)value)->ifr_name,selected==&ap?"ap1":"st1"));
    if (bind_failure) return -1;
    ++ap_bound_before_bind; return 0;
}
int __real_lwip_bind(int fd,const struct sockaddr *address,socklen_t length) {
    ++binding_calls;
    if (fd==3 || fd==4 || missing_hook) { assert(address->sa_family==AF_INET && ((const struct sockaddr_in*)address)->sin_addr.s_addr==0); return 0; }
    uint32_t expected=htonl(fd==1?(selected==&ap?0xc0a80401u:0xc0a8322au):0x7f000001u);
    if (fd==1) assert(ap_bound_before_bind==1);
    if (address->sa_family==AF_INET) {
        assert(length==sizeof(struct sockaddr_in));
        assert(((const struct sockaddr_in*)address)->sin_addr.s_addr==expected);
    } else {
        assert(length==sizeof(struct sockaddr_in6));
        const unsigned char *bytes=((const struct sockaddr_in6*)address)->sin6_addr.bytes;
        for(unsigned i=0;i<10;i++) assert(bytes[i]==0);
        assert(bytes[10]==255 && bytes[11]==255 && !memcmp(bytes+12,&expected,4));
    }
    return fd==2&&fail_control?-1:0;
}
esp_err_t httpd_start(httpd_handle_t *server,const httpd_config_t *cfg) {
    ++http_started;
    struct sockaddr_in unrelated={.sin_family=AF_INET,.sin_port=htons(53)};
    assert(__wrap_lwip_bind(3,(struct sockaddr*)&unrelated,sizeof(unrelated))==0);
    unrelated.sin_port=htons(cfg->server_port);
    current=&other_id;
    assert(__wrap_lwip_bind(4,(struct sockaddr*)&unrelated,sizeof(unrelated))==0);
    current=&owner_id;
    int result;
    if (missing_hook) result=__real_lwip_bind(1,(struct sockaddr*)&unrelated,sizeof(unrelated));
    else if (ipv6) {
        struct sockaddr_in6 listener={.sin6_family=AF_INET6,.sin6_port=htons(cfg->server_port)};
        result=__wrap_lwip_bind(1,(struct sockaddr*)&listener,sizeof(listener));
    } else result=__wrap_lwip_bind(1,(struct sockaddr*)&unrelated,sizeof(unrelated));
    if(result!=0) return ESP_FAIL;
    if (ipv6) {
        struct sockaddr_in6 control={.sin6_family=AF_INET6,.sin6_port=htons(cfg->ctrl_port)};
        result=__wrap_lwip_bind(2,(struct sockaddr*)&control,sizeof(control));
    } else {
        struct sockaddr_in control={.sin_family=AF_INET,.sin_port=htons(cfg->ctrl_port)};
        result=missing_hook?__real_lwip_bind(2,(struct sockaddr*)&control,sizeof(control)):
                           __wrap_lwip_bind(2,(struct sockaddr*)&control,sizeof(control));
    }
    if(result!=0) return ESP_FAIL;
    *server=&server_id; return ESP_OK;
}
esp_err_t httpd_stop(httpd_handle_t server) { assert(server==&server_id); ++stopped; return ESP_OK; }
static void reset(void) {
    current=&owner_id; selected=NULL; up=true;
    bad_ip=bind_failure=missing_hook=ipv6=fail_control=missing_interface=wrong_socket_type=false;
    binding_calls=stopped=http_started=ap_bound_before_bind=0; now_us=0;
}
int main(void) {
    httpd_config_t cfg={.server_port=80,.ctrl_port=32768}; httpd_handle_t server;
    reset(); assert(tracker_httpd_start_ap_only(&server,&cfg)==ESP_OK && server==&server_id && binding_calls==4);
    reset(); ipv6=true; assert(tracker_httpd_start_ap_only(&server,&cfg)==ESP_OK && ap_bound_before_bind==1);
    reset(); bind_failure=true; assert(tracker_httpd_start_ap_only(&server,&cfg)!=ESP_OK && server==NULL && binding_calls==2);
    reset(); fail_control=true; assert(tracker_httpd_start_ap_only(&server,&cfg)!=ESP_OK && server==NULL);
    reset(); missing_hook=true; assert(tracker_httpd_start_ap_only(&server,&cfg)==ESP_ERR_INVALID_STATE && server==NULL && stopped==1);
    reset(); up=false; assert(tracker_httpd_start_ap_only(&server,&cfg)!=ESP_OK && http_started==0 && now_us==3000000);
    reset(); bad_ip=true; assert(tracker_httpd_start_ap_only(&server,&cfg)!=ESP_OK && http_started==0);
    reset(); assert(tracker_httpd_start_ap_only(NULL,&cfg)==ESP_ERR_INVALID_ARG);
    assert(tracker_httpd_start_ap_only(&server,NULL)==ESP_ERR_INVALID_ARG);
    const char *keys[]={"WIFI_AP_DEF","WIFI_STA_DEF"};
    const uint32_t addresses[]={0xc0a80401u,0xc0a8322au};
    for(unsigned i=0;i<2;++i) {
        reset(); assert(tracker_httpd_start_wifi(&server,&cfg,keys[i],htonl(addresses[i]))==ESP_OK);
        assert(server==&server_id && binding_calls==4 && ap_bound_before_bind==1);
        /* Outside the startup owner window, the same port is untouched. */
        struct sockaddr_in outside={.sin_family=AF_INET,.sin_port=htons(80)};
        assert(__wrap_lwip_bind(4,(struct sockaddr*)&outside,sizeof(outside))==0);
        assert(ap_bound_before_bind==1);
        reset(); ipv6=true;
        assert(tracker_httpd_start_wifi(&server,&cfg,keys[i],htonl(addresses[i]))==ESP_OK && ap_bound_before_bind==1);
        reset(); bind_failure=true;
        assert(tracker_httpd_start_wifi(&server,&cfg,keys[i],htonl(addresses[i]))!=ESP_OK && server==NULL);
        reset(); wrong_socket_type=true;
        assert(tracker_httpd_start_wifi(&server,&cfg,keys[i],htonl(addresses[i]))!=ESP_OK && server==NULL && ap_bound_before_bind==0);
        reset(); fail_control=true;
        assert(tracker_httpd_start_wifi(&server,&cfg,keys[i],htonl(addresses[i]))!=ESP_OK && server==NULL);
        reset(); missing_hook=true;
        assert(tracker_httpd_start_wifi(&server,&cfg,keys[i],htonl(addresses[i]))==ESP_ERR_INVALID_STATE && stopped==1);
        reset(); up=false;
        assert(tracker_httpd_start_wifi(&server,&cfg,keys[i],htonl(addresses[i]))!=ESP_OK && http_started==0 && now_us==3000000);
        reset(); missing_interface=true;
        assert(tracker_httpd_start_wifi(&server,&cfg,keys[i],htonl(addresses[i]))!=ESP_OK && http_started==0 && now_us==0);
        reset(); assert(tracker_httpd_start_wifi(&server,&cfg,keys[i],htonl(addresses[1-i]))!=ESP_OK && http_started==0);
        reset(); assert(tracker_httpd_start_wifi(&server,&cfg,keys[i],htonl(0xc0a8322bu))!=ESP_OK && http_started==0);
    }
    const char *rejected[]={NULL,"","PPP_DEF","ETH_DEF","WIFI_STA_DEF.evil","wifi_sta_def"};
    for(unsigned i=0;i<sizeof(rejected)/sizeof(rejected[0]);++i) {
        reset(); assert(tracker_httpd_start_wifi(&server,&cfg,rejected[i],htonl(0xc0a8322au))==ESP_ERR_INVALID_ARG);
        assert(http_started==0 && binding_calls==0);
    }
    reset(); assert(tracker_httpd_start_wifi(&server,&cfg,"WIFI_STA_DEF",0)==ESP_ERR_INVALID_ARG);
    httpd_config_t no_port=cfg; no_port.server_port=0;
    assert(tracker_httpd_start_wifi(&server,&no_port,"WIFI_STA_DEF",htonl(0xc0a8322au))==ESP_ERR_INVALID_ARG);
    no_port=cfg; no_port.ctrl_port=0;
    assert(tracker_httpd_start_wifi(&server,&no_port,"WIFI_STA_DEF",htonl(0xc0a8322au))==ESP_ERR_INVALID_ARG);
    puts("setup listener: AP/STA address/interface isolation, loopback control and failure tests passed");
}
