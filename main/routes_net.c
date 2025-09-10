// Networking REST endpoints (phase1 skeleton)
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "opdi_net.h"
#include "esp_wifi.h" // for scan/types
#include "cJSON.h"
#include "esp_idf_version.h"
#include "opdi_cam.h"

// Internal metrics/logs access
extern void opdi_net_get_metrics(opdi_net_metrics_t *out);
extern size_t opdi_net_logs_serialize(char *out, size_t cap);
__attribute__((weak)) void opdi_net_internal_scan_account(void); // from opdi_net.c

static const char *TAG = "routes_net";

static const char *state_str(opdi_net_state_t s){
    switch(s){
        case NET_STA_CONNECTED: return "STA_CONNECTED";
        case NET_AP_ACTIVE: return "AP_ACTIVE";
        case NET_STA_CONNECT: return "STA_CONNECT";
        default: return "INIT";
    }
}

// Use public accessor functions instead of weak variable references
#define opdi_net_cached_ip opdi_net_ip_cached
#define opdi_net_cached_gw opdi_net_gw_cached
extern esp_err_t opdi_net_ap_set(const char*, uint8_t); // already declared
extern void opdi_net_ap_get(char*, size_t, uint8_t*);
static void build_status_json(char *buf, size_t cap){
    if (!buf || cap<16) return;
    const opdi_net_state_t st = opdi_net_get_state();
    int rssi_val = 0; bool have=false; char ip_field[48]="null"; char gw_field[48]="null";
    if (st == NET_STA_CONNECTED){
        wifi_ap_record_t ap; if (esp_wifi_sta_get_ap_info(&ap)==ESP_OK){ rssi_val=ap.rssi; have=true; }
        const char *cip=opdi_net_cached_ip(); if(cip&&cip[0]) snprintf(ip_field,sizeof(ip_field),"\"%s\"",cip);
        const char *cgw=opdi_net_cached_gw(); if(cgw&&cgw[0]) snprintf(gw_field,sizeof(gw_field),"\"%s\"",cgw);
    }
    opdi_net_metrics_t m; opdi_net_get_metrics(&m);
    if (st == NET_AP_ACTIVE){
        char ap_ssid[33]; uint8_t ch=0; opdi_net_ap_get(ap_ssid,sizeof(ap_ssid),&ch);
        snprintf(buf, cap, "{\"state\":\"%s\",\"mode\":\"ap\",\"ap\":{\"ssid\":\"%s\",\"channel\":%u},\"metrics\":{\"attempts\":%lu,\"success\":%lu,\"avg_ms\":%lu,\"scans\":%lu,\"retries\":%lu}}", state_str(st), ap_ssid, ch,
                 (unsigned long)m.connect_attempts,
                 (unsigned long)m.connects_success,
                 (unsigned long)m.avg_connect_time_ms,
                 (unsigned long)m.scan_count,
                 (unsigned long)m.current_retries);
    } else if (have){
        snprintf(buf, cap, "{\"state\":\"%s\",\"ip\":%s,\"gw\":%s,\"rssi\":%d,\"metrics\":{\"attempts\":%lu,\"success\":%lu,\"avg_ms\":%lu,\"scans\":%lu,\"retries\":%lu}}", state_str(st), ip_field, gw_field, rssi_val,
                 (unsigned long)m.connect_attempts,
                 (unsigned long)m.connects_success,
                 (unsigned long)m.avg_connect_time_ms,
                 (unsigned long)m.scan_count,
                 (unsigned long)m.current_retries);
    } else {
        snprintf(buf, cap, "{\"state\":\"%s\",\"ip\":null,\"gw\":null,\"rssi\":null,\"metrics\":{\"attempts\":%lu,\"success\":%lu,\"avg_ms\":%lu,\"scans\":%lu,\"retries\":%lu}}", state_str(st),
                 (unsigned long)m.connect_attempts,
                 (unsigned long)m.connects_success,
                 (unsigned long)m.avg_connect_time_ms,
                 (unsigned long)m.scan_count,
                 (unsigned long)m.current_retries);
    }
}
static esp_err_t net_status_get(httpd_req_t *req){ char buf[512]; build_status_json(buf,sizeof(buf)); httpd_resp_set_type(req,"application/json"); httpd_resp_sendstr(req,buf); return ESP_OK; }

// Profiles list (enumeration)
extern size_t opdi_net_profiles_serialize(char *out, size_t out_cap);
static esp_err_t net_profiles_get(httpd_req_t *req){
    static char buf[1024];
    size_t n = opdi_net_profiles_serialize(buf, sizeof(buf));
    if (n==0) { strcpy(buf, "[]"); n=2; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

// Add/update profile
static esp_err_t net_profiles_post(httpd_req_t *req){
    char content[384];
    int received = httpd_req_recv(req, content, sizeof(content)-1);
    if (received <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body"); return ESP_OK; }
    content[received] = '\0';
    cJSON *root = cJSON_Parse(content);
    if (!root){ httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json"); return ESP_OK; }
    cJSON *j_ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    if (!cJSON_IsString(j_ssid) || j_ssid->valuestring[0]=='\0') { cJSON_Delete(root); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required"); return ESP_OK; }
    cJSON *j_psk = cJSON_GetObjectItemCaseSensitive(root, "psk");
    cJSON *j_hidden = cJSON_GetObjectItemCaseSensitive(root, "hidden");
    cJSON *j_auth = cJSON_GetObjectItemCaseSensitive(root, "auth");
    opdi_net_profile_t p = {0};
    strlcpy(p.ssid, j_ssid->valuestring, sizeof(p.ssid));
    if (cJSON_IsString(j_psk)) strlcpy(p.psk, j_psk->valuestring, sizeof(p.psk));
    p.hidden = cJSON_IsBool(j_hidden) ? cJSON_IsTrue(j_hidden) : false;
    p.auth = (cJSON_IsNumber(j_auth) && j_auth->valuedouble>=0) ? (uint8_t)j_auth->valuedouble : 0;
    esp_err_t err = opdi_net_add_profile(&p);
    cJSON_Delete(root);
    if (err!=ESP_OK){ httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "add fail"); return ESP_OK; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// Delete profile by hash tail (id path segment)
extern esp_err_t opdi_net_forget_hash_tail(const char *partial_id);
static esp_err_t net_profiles_delete(httpd_req_t *req){
    const char *uri = req->uri;
    const char *id = strrchr(uri, '/');
    if(!id || strlen(id)<2){ httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id"); return ESP_OK; }
    id++;
    esp_err_t err = opdi_net_forget_hash_tail(id);
    if (err!=ESP_OK){ httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found"); return ESP_OK; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// Trigger connect
static esp_err_t net_connect_post(httpd_req_t *req){
    char content[160]; int received = httpd_req_recv(req, content, sizeof(content)-1);
    const char *ssid = NULL; content[0]='\0';
    if (received > 0){
        content[received]='\0';
        cJSON *root = cJSON_Parse(content);
        if (root){
            cJSON *j_ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
            if (cJSON_IsString(j_ssid) && j_ssid->valuestring[0]) ssid = j_ssid->valuestring;
        }
        // We don't need to keep JSON DOM after extracting pointer, copy if present
        static char ssid_copy[33];
        if (ssid){ strlcpy(ssid_copy, ssid, sizeof(ssid_copy)); ssid = ssid_copy; }
        if (root) cJSON_Delete(root);
    }
    opdi_net_connect(ssid);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// Scan (placeholder returns empty array)
static esp_err_t net_scan_get(httpd_req_t *req){
    httpd_resp_set_type(req, "application/json");
    // Perform a blocking active scan (Phase1 simple). Limit records to e.g. 20.
    wifi_scan_config_t cfg = {0};
    cfg.show_hidden = true; // allow discovering hidden if broadcasting at moment
    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    esp_err_t err = esp_wifi_scan_start(&cfg, true); // block until done
    if (err != ESP_OK){
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }
    if (opdi_net_internal_scan_account) opdi_net_internal_scan_account();
    uint16_t ap_count = 0; esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 32) ap_count = 32; // cap
    wifi_ap_record_t *recs = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!recs){ httpd_resp_sendstr(req, "[]"); return ESP_OK; }
    if (esp_wifi_scan_get_ap_records(&ap_count, recs)!=ESP_OK){ free(recs); httpd_resp_sendstr(req, "[]"); return ESP_OK; }
    static char json[2048]; size_t len = 0; json[len++]='['; json[len]='\0';
    for (uint16_t i=0;i<ap_count;i++) {
        if (recs[i].rssi < CONFIG_OPDI_NET_SCAN_MIN_RSSI) continue;
        char entry[160];
        // auth mode numeric, channel, rssi
        const uint8_t *b = recs[i].bssid;
        int n = snprintf(entry, sizeof(entry),
            "{\"ssid\":\"%.*s\",\"bssid\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"rssi\":%d,\"auth\":%d,\"ch\":%u},",
            32, (const char*)recs[i].ssid, b[0],b[1],b[2],b[3],b[4],b[5], recs[i].rssi, recs[i].authmode, recs[i].primary);
        if (n <= 0) continue;
        if (len + (size_t)n >= sizeof(json)-2) break; // leave room for closing
        memcpy(json+len, entry, n); len += n; json[len]='\0';
    }
    if (len>1 && json[len-1]==',') { json[len-1]=']'; json[len]='\0'; }
    else { json[len++]=']'; json[len]='\0'; }
    httpd_resp_send(req, json, len);
    free(recs);
    return ESP_OK;
}

// Quick scan summary (top 16 by RSSI) - performs a fresh scan (blocking) similar to net_scan_get but trims to 16 sorted results.
static esp_err_t net_scan_summary_get(httpd_req_t *req){
    httpd_resp_set_type(req, "application/json");
    wifi_scan_config_t cfg = {0};
    cfg.show_hidden = true; cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK){ httpd_resp_sendstr(req, "[]"); return ESP_OK; }
    if (opdi_net_internal_scan_account) opdi_net_internal_scan_account();
    uint16_t ap_total = 0; esp_wifi_scan_get_ap_num(&ap_total);
    if (ap_total == 0){ httpd_resp_sendstr(req, "[]"); return ESP_OK; }
    if (ap_total > 64) ap_total = 64; // temp cap
    wifi_ap_record_t *recs = calloc(ap_total, sizeof(wifi_ap_record_t));
    if (!recs){ httpd_resp_sendstr(req, "[]"); return ESP_OK; }
    if (esp_wifi_scan_get_ap_records(&ap_total, recs)!=ESP_OK){ free(recs); httpd_resp_sendstr(req, "[]"); return ESP_OK; }
    // Simple selection sort for top 16 by RSSI (descending)
    uint16_t limit = ap_total < 16 ? ap_total : 16;
    for (uint16_t i=0;i<limit;i++){
        uint16_t max_i = i; for (uint16_t j=i+1;j<ap_total;j++){ if (recs[j].rssi > recs[max_i].rssi) max_i = j; }
        if (max_i != i){ wifi_ap_record_t tmp = recs[i]; recs[i]=recs[max_i]; recs[max_i]=tmp; }
    }
    static char json[1600]; size_t len=0; json[len++]='['; json[len]='\0';
    for (uint16_t i=0;i<limit;i++){
        if (recs[i].rssi < CONFIG_OPDI_NET_SCAN_MIN_RSSI) continue;
        char entry[128];
        int n = snprintf(entry,sizeof(entry),"{\"ssid\":\"%.*s\",\"rssi\":%d,\"auth\":%d,\"ch\":%u},",32,(const char*)recs[i].ssid,recs[i].rssi,recs[i].authmode,recs[i].primary);
        if (n<=0) continue;
        if (len + (size_t)n >= sizeof(json)-2) break;
        memcpy(json+len, entry, n);
        len+=n;
        json[len]='\0';
    }
    if (len>1 && json[len-1]==',') json[len-1]=']'; else { json[len++]=']'; json[len]='\0'; }
    httpd_resp_send(req, json, len);
    free(recs);
    return ESP_OK;
}

// Metrics endpoint (explicit)
static esp_err_t net_metrics_get(httpd_req_t *req){
    opdi_net_metrics_t m; opdi_net_get_metrics(&m);
    char buf[192];
    // Cast to unsigned long to satisfy format expectations under -Werror=format (riscv32 toolchain typedefs)
    snprintf(buf, sizeof(buf), "{\"attempts\":%lu,\"success\":%lu,\"avg_ms\":%lu,\"scans\":%lu,\"retries\":%lu}",
             (unsigned long)m.connect_attempts,
             (unsigned long)m.connects_success,
             (unsigned long)m.avg_connect_time_ms,
             (unsigned long)m.scan_count,
             (unsigned long)m.current_retries);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

// Logs endpoint
static esp_err_t net_logs_get(httpd_req_t *req){
    static char buf[1024]; size_t n = opdi_net_logs_serialize(buf, sizeof(buf));
    if (!n){ httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "log buf"); return ESP_OK; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

// AP config override (not persisted yet) - placeholder
static esp_err_t net_ap_config_post(httpd_req_t *req){
    char content[256]; int received = httpd_req_recv(req, content, sizeof(content)-1);
    const char *new_ssid=NULL; int new_ch=-1;
    if (received>0){
        content[received]='\0';
        cJSON *root = cJSON_Parse(content);
        if (root){
            cJSON *j_ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
            cJSON *j_ch = cJSON_GetObjectItemCaseSensitive(root, "channel");
            if (cJSON_IsString(j_ssid) && j_ssid->valuestring[0]) new_ssid = j_ssid->valuestring;
            if (cJSON_IsNumber(j_ch)) new_ch = (int)j_ch->valuedouble;
            static char ssid_copy[33];
            if (new_ssid){ strlcpy(ssid_copy, new_ssid, sizeof(ssid_copy)); new_ssid = ssid_copy; }
            cJSON_Delete(root);
        }
    }
    if (opdi_net_ap_set(new_ssid, (uint8_t)new_ch)!=ESP_OK){ httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "persist"); return ESP_OK; }
    char ssid_now[33]; uint8_t ch_now; opdi_net_ap_get(ssid_now, sizeof(ssid_now), &ch_now);
    char resp[128]; snprintf(resp, sizeof(resp), "{\"ok\":true,\"ssid\":\"%s\",\"channel\":%u}", ssid_now, ch_now);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t net_ap_config_get(httpd_req_t *req){
    char ssid[33]; uint8_t ch=0; opdi_net_ap_get(ssid, sizeof(ssid), &ch);
    char resp[128]; snprintf(resp, sizeof(resp), "{\"ssid\":\"%s\",\"channel\":%u}", ssid, ch);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// Version endpoint: host IDF, app build (compile time), hosted FW version (if captured)
static esp_err_t net_version_get(httpd_req_t *req){
    const char *hosted = opdi_net_hosted_fw_version(); if (!hosted) hosted="";
    char buf[160];
    snprintf(buf,sizeof(buf),"{\"idf\":\"%s\",\"app_build\":\"%s %s\",\"hosted_fw\":\"%s\"}", IDF_VER, __DATE__, __TIME__, hosted);
    httpd_resp_set_type(req,"application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

// ---- Camera endpoint handlers (integrated with opdi_cam module) ----
static esp_err_t cam_snapshot_get(httpd_req_t *req){
    int need = opdi_cam_snapshot(NULL, 0);
    if (need <= 0){ httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "snap" ); return ESP_OK; }
    unsigned char buf[256]; // Phase1 stub size
    if (need > (int)sizeof(buf)){ httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "snap_big"); return ESP_OK; }
    int got = opdi_cam_snapshot(buf, sizeof(buf));
    if (got != need){ httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "snap_mismatch"); return ESP_OK; }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, (const char*)buf, got);
    return ESP_OK;
}
static esp_err_t cam_config_get(httpd_req_t *req){
    opdi_cam_config_t c; opdi_cam_get_config(&c);
    char resp[160];
    snprintf(resp,sizeof(resp),"{\"brightness\":%d,\"contrast\":%d,\"saturation\":%d,\"auto_exposure\":%s}",
        c.brightness,c.contrast,c.saturation,c.auto_exposure?"true":"false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}
static esp_err_t cam_config_post(httpd_req_t *req){
    char content[256]; int received = httpd_req_recv(req, content, sizeof(content)-1);
    if (received<=0){ httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_OK; }
    content[received]='\0';
    cJSON *root = cJSON_Parse(content);
    if(!root){ httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json"); return ESP_OK; }
    opdi_cam_config_t c; opdi_cam_get_config(&c);
    cJSON *j_b = cJSON_GetObjectItemCaseSensitive(root,"brightness");
    cJSON *j_c = cJSON_GetObjectItemCaseSensitive(root,"contrast");
    cJSON *j_s = cJSON_GetObjectItemCaseSensitive(root,"saturation");
    cJSON *j_ae = cJSON_GetObjectItemCaseSensitive(root,"auto_exposure");
    if (cJSON_IsNumber(j_b)) c.brightness = (int)j_b->valuedouble;
    if (cJSON_IsNumber(j_c)) c.contrast = (int)j_c->valuedouble;
    if (cJSON_IsNumber(j_s)) c.saturation = (int)j_s->valuedouble;
    if (cJSON_IsBool(j_ae)) c.auto_exposure = cJSON_IsTrue(j_ae);
    cJSON_Delete(root);
    if (opdi_cam_set_config(&c)!=ESP_OK){ httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "persist"); return ESP_OK; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

void routes_net_register(httpd_handle_t server){
    const httpd_uri_t endpoints[] = {
        { .uri="/api/v1/net/sta/status",    .method=HTTP_GET,  .handler=net_status_get, .user_ctx=NULL },
        { .uri="/api/v1/net/sta/profiles",  .method=HTTP_GET,  .handler=net_profiles_get, .user_ctx=NULL },
        { .uri="/api/v1/net/sta/profiles",  .method=HTTP_POST, .handler=net_profiles_post, .user_ctx=NULL },
        { .uri="/api/v1/net/sta/connect",   .method=HTTP_POST, .handler=net_connect_post, .user_ctx=NULL },
        { .uri="/api/v1/net/scan",          .method=HTTP_GET,  .handler=net_scan_get, .user_ctx=NULL },
    { .uri="/api/v1/net/scan_summary",  .method=HTTP_GET,  .handler=net_scan_summary_get, .user_ctx=NULL },
        { .uri="/api/v1/net/ap/config",     .method=HTTP_POST, .handler=net_ap_config_post, .user_ctx=NULL },
    { .uri="/api/v1/net/ap/config",     .method=HTTP_GET,  .handler=net_ap_config_get, .user_ctx=NULL },
    { .uri="/api/v1/net/version",       .method=HTTP_GET,  .handler=net_version_get, .user_ctx=NULL },
    { .uri="/api/v1/net/metrics",       .method=HTTP_GET,  .handler=net_metrics_get, .user_ctx=NULL },
    { .uri="/api/v1/net/logs",          .method=HTTP_GET,  .handler=net_logs_get, .user_ctx=NULL },
    // camera
    { .uri="/api/v1/cam/snapshot",      .method=HTTP_GET,  .handler=cam_snapshot_get, .user_ctx=NULL },
    { .uri="/api/v1/cam/config",        .method=HTTP_GET,  .handler=cam_config_get, .user_ctx=NULL },
    { .uri="/api/v1/cam/config",        .method=HTTP_POST, .handler=cam_config_post, .user_ctx=NULL },
        // dynamic delete registration: pattern not directly supported; simple reuse with wildcard style not built-in -> register generic handler and parse
        { .uri="/api/v1/net/sta/profiles/", .method=HTTP_DELETE, .handler=net_profiles_delete, .user_ctx=NULL },
    };
    for (size_t i=0;i<sizeof(endpoints)/sizeof(endpoints[0]);++i){
        httpd_register_uri_handler(server, &endpoints[i]);
    }
    ESP_LOGI(TAG, "net routes registered");
}
