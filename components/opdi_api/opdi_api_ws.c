#include "opdi_api_ws.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi_types.h" // for wifi_err_reason_t values (guarded cases below)
#include "opdi_net.h" // for opdi_net_metrics_t and accessors
#include <string.h>

static const char *TAG = "opdi_ws";

#ifndef CONFIG_HTTPD_WS_SUPPORT
#warning "HTTPD WS support not enabled; websocket API disabled"
#endif

typedef struct ws_client_node {
    int fd;
    struct ws_client_node *next;
} ws_client_node_t;
static ws_client_node_t *s_clients = NULL;
static httpd_handle_t s_server = NULL;
static size_t s_client_count = 0;

static void add_client(int fd){
    ws_client_node_t *n = (ws_client_node_t*)calloc(1,sizeof(*n));
    if(!n) return;
    n->fd = fd;
    n->next = s_clients;
    s_clients = n;
    s_client_count++;
}
static void remove_client(int fd){
    ws_client_node_t **pp=&s_clients; while(*pp){ if((*pp)->fd==fd){ ws_client_node_t *d=*pp; *pp=d->next; free(d); s_client_count--; return;} pp=&(*pp)->next; } }

void opdi_api_ws_broadcast(const char *json, size_t len){
#if CONFIG_HTTPD_WS_SUPPORT
    if(!json||!s_server) return;
    ws_client_node_t *n=s_clients; while(n){
        httpd_ws_frame_t f={0}; f.payload=(uint8_t*)json; f.len=len; f.type=HTTPD_WS_TYPE_TEXT;
        if (httpd_ws_send_frame_async(s_server, n->fd, &f)!=ESP_OK){ ESP_LOGW(TAG,"ws send fail fd=%d -> drop", n->fd); int bad=n->fd; n=n->next; remove_client(bad); continue; }
        n=n->next;
    }
#else
    (void)json; (void)len;
#endif
}

// Hooks overriding weak symbols from opdi_net (now we directly use public accessors)
void opdi_net_emit_sta_connected(const char *ip, int rssi, const uint8_t bssid[6]){
    const char *gw = opdi_net_gw_cached();
    char buf[192]; snprintf(buf,sizeof(buf),"{\"type\":\"net\",\"sub\":\"sta_connected\",\"ip\":\"%s\",\"gw\":\"%s\",\"rssi\":%d}", ip?ip:"", gw?gw:"", rssi);
    opdi_api_ws_broadcast(buf, strlen(buf));
    ESP_LOGI(TAG, "broadcast sta_connected");
}
void opdi_net_emit_sta_disconnected(int reason){
    const char *reason_text="unknown";
    switch(reason){
#ifdef WIFI_REASON_AUTH_FAIL
    case WIFI_REASON_AUTH_FAIL: reason_text="auth_fail"; break;
#endif
#ifdef WIFI_REASON_NO_AP_FOUND
    case WIFI_REASON_NO_AP_FOUND: reason_text="no_ap"; break;
#endif
#ifdef WIFI_REASON_ASSOC_LEAVE
    case WIFI_REASON_ASSOC_LEAVE: reason_text="left"; break;
#endif
#ifdef WIFI_REASON_BEACON_TIMEOUT
    case WIFI_REASON_BEACON_TIMEOUT: reason_text="beacon_timeout"; break;
#endif
#ifdef WIFI_REASON_HANDSHAKE_TIMEOUT
    case WIFI_REASON_HANDSHAKE_TIMEOUT: reason_text="hs_timeout"; break;
#endif
    default: break;
    }
    char buf[128]; snprintf(buf,sizeof(buf),"{\"type\":\"net\",\"sub\":\"sta_disconnected\",\"reason\":%d,\"reason_text\":\"%s\"}", reason, reason_text);
    opdi_api_ws_broadcast(buf, strlen(buf));
    ESP_LOGI(TAG, "broadcast sta_disconnected");
}
void opdi_net_emit_ap_active(const char *ssid, uint8_t channel){
    char buf[160]; snprintf(buf,sizeof(buf),"{\"type\":\"net\",\"sub\":\"ap_active\",\"ssid\":\"%s\",\"channel\":%u}", ssid?ssid:"", channel);
    opdi_api_ws_broadcast(buf, strlen(buf));
    ESP_LOGI(TAG, "broadcast ap_active");
}

// Metrics emission (overrides weak hook). Uses public metrics accessor.
void opdi_net_emit_metrics(void){
    opdi_net_metrics_t m; opdi_net_get_metrics(&m);
    char buf[256];
    snprintf(buf,sizeof(buf),"{\"type\":\"net\",\"sub\":\"metrics\",\"attempts\":%lu,\"success\":%lu,\"avg_ms\":%lu,\"scans\":%lu,\"retries\":%lu}",
        (unsigned long)m.connect_attempts,
        (unsigned long)m.connects_success,
        (unsigned long)m.avg_connect_time_ms,
        (unsigned long)m.scan_count,
        (unsigned long)m.current_retries);
    opdi_api_ws_broadcast(buf, strlen(buf));
    ESP_LOGI(TAG, "broadcast metrics");
}

#if CONFIG_HTTPD_WS_SUPPORT
static esp_err_t ws_handler(httpd_req_t *req){
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "handshake established, fd=%d", httpd_req_to_sockfd(req));
        add_client(httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    httpd_ws_frame_t frame={0}; frame.type=HTTPD_WS_TYPE_TEXT;
    if (httpd_ws_recv_frame(req, &frame, 0)!=ESP_OK) return ESP_FAIL;
    if (frame.len){
        frame.payload = malloc(frame.len+1); if(!frame.payload) return ESP_ERR_NO_MEM;
        if (httpd_ws_recv_frame(req, &frame, frame.len+1)==ESP_OK){ frame.payload[frame.len]='\0'; ESP_LOGI(TAG, "ws rx (%u bytes)", (unsigned)frame.len); }
        free(frame.payload);
    }
    return ESP_OK;
}
#endif

void opdi_api_ws_register(httpd_handle_t server){
    s_server = server;
#if CONFIG_HTTPD_WS_SUPPORT
    httpd_uri_t u_ws = { .uri="/ws", .method=HTTP_GET, .handler=ws_handler, .is_websocket=true };
    httpd_register_uri_handler(server, &u_ws);
    ESP_LOGI(TAG, "WebSocket endpoint /ws ready");
#else
    ESP_LOGW(TAG, "HTTPD_WS_SUPPORT disabled; /ws not registered");
#endif
}
