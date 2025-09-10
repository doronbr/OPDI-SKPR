#ifndef PROJECT_VER
#define PROJECT_VER "0.1.0"
#endif


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "opdi_net.h"
#include "opdi_api_ws.h"
// Static UI registration
void opdi_api_static_register(httpd_handle_t);

// Forward declare route registration
void routes_net_register(httpd_handle_t server);

static const char *TAG = "app";

static esp_err_t root_get(httpd_req_t *r) {
  httpd_resp_sendstr(r, "OK");
  return ESP_OK;
}
static esp_err_t sysinfo_get(httpd_req_t *r) {
  char buf[256];
  int n = snprintf(buf, sizeof(buf),
    "{\"device\":\"OPDI_SKPR\",\"fw_version\":\"%s\",\"idf_version\":\"%s\",\"uptime_s\":%ld}",
    PROJECT_VER, esp_get_idf_version(), (long)(esp_timer_get_time()/1000000LL));
  httpd_resp_set_type(r, "application/json");
  httpd_resp_send(r, buf, n);
  return ESP_OK;
}
static httpd_handle_t start_httpd(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    httpd_handle_t h=NULL;
    if (httpd_start(&h,&cfg)!=ESP_OK) return NULL;
    httpd_uri_t u_root = { .uri="/", .method=HTTP_GET, .handler=root_get };
    httpd_uri_t u_sys  = { .uri="/api/v1/system/info", .method=HTTP_GET, .handler=sysinfo_get };
    httpd_register_uri_handler(h, &u_root);
    httpd_register_uri_handler(h, &u_sys);
  // register networking routes + websocket + static UI
    routes_net_register(h);
    opdi_api_ws_register(h);
  opdi_api_static_register(h);
    return h;
}
void app_main(void) {
  ESP_ERROR_CHECK(nvs_flash_init());
  // Initialize network manager (will start hosted link etc.)
  opdi_net_init();
  start_httpd();
  ESP_LOGI(TAG, "OPDI_SKPR up (net + httpd)");
  while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}