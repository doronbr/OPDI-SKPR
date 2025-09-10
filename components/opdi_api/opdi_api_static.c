// Simple static file serving for Web UI (Phase 1): reads from SPIFFS path /spiffs/web
// Files expected: /spiffs/web/index.html, /spiffs/web/app.js, /spiffs/web/style.css

#include "esp_http_server.h"
#include "esp_log.h"
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

static const char *TAG = "opdi_ui";

static esp_err_t send_file(httpd_req_t *r, const char *rel, const char *ctype){
    char path[128]; snprintf(path, sizeof(path), "/spiffs/web/%s", rel);
    FILE *f = fopen(path, "rb");
    if(!f){ ESP_LOGW(TAG, "open %s fail: %s", path, strerror(errno)); return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "not found"); }
    httpd_resp_set_type(r, ctype);
    char buf[1024]; size_t n;
    while((n=fread(buf,1,sizeof(buf),f))>0){ if (httpd_resp_send_chunk(r, buf, n)!=ESP_OK){ fclose(f); httpd_resp_send_chunk(r, NULL, 0); return ESP_FAIL; } }
    fclose(f);
    httpd_resp_send_chunk(r, NULL, 0);
    return ESP_OK;
}

static esp_err_t ui_index(httpd_req_t *r){ return send_file(r, "index.html", "text/html; charset=utf-8"); }
static esp_err_t ui_appjs(httpd_req_t *r){ return send_file(r, "app.js", "application/javascript"); }
static esp_err_t ui_style(httpd_req_t *r){ return send_file(r, "style.css", "text/css; charset=utf-8"); }

// Register static handlers (called from app_main after httpd start)
void opdi_api_static_register(httpd_handle_t h){
    httpd_uri_t u_index = { .uri="/", .method=HTTP_GET, .handler=ui_index };
    httpd_register_uri_handler(h, &u_index);
    httpd_uri_t u_app = { .uri="/app.js", .method=HTTP_GET, .handler=ui_appjs };
    httpd_register_uri_handler(h, &u_app);
    httpd_uri_t u_css = { .uri="/style.css", .method=HTTP_GET, .handler=ui_style };
    httpd_register_uri_handler(h, &u_css);
    ESP_LOGI(TAG, "UI static routes mounted");
}

// Provide weak symbol in case not linked
__attribute__((weak)) void opdi_api_static_register(httpd_handle_t h);