// Minimal hosted link abstraction layer (stubs) - phase 1
#include "esp_log.h"
#include "esp_wifi.h"
#include <string.h>

static const char *TAG = "opdi_net_hosted";

// Weak symbol; replace when wiring ESP-Hosted host library
__attribute__((weak))
void opdi_net_hosted_start(void) {
    // TODO: init ESP-Hosted bus (SPI/SDIO/UART), bring up NWP, create netif
    ESP_LOGI(TAG, "ESP-Hosted link start (bus=%s)", CONFIG_OPDI_HOSTED_BUS);
    // wifi init skeleton (hosted will own actual radio). Keep placeholders.
    static bool s_wifi_inited = false;
    if (!s_wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&cfg)!=ESP_OK) {
            ESP_LOGW(TAG, "wifi init placeholder failed");
        } else {
            s_wifi_inited = true;
        }
    } else {
        ESP_LOGD(TAG, "wifi already inited (hosted stub)");
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
}

// Start provisioning AP (open) - placeholder stub
__attribute__((weak))
esp_err_t opdi_net_hosted_start_ap(const char *ssid, uint8_t channel) {
    wifi_config_t ap = {0};
    strlcpy((char*)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
    ap.ap.channel = channel;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    ESP_LOGI(TAG, "start AP ssid=%s ch=%u", ssid, channel);
    return ESP_OK;
}

// Attempt station connection with provided credentials - placeholder
__attribute__((weak))
esp_err_t opdi_net_hosted_connect_sta(const char *ssid, const char *psk, bool hidden) {
    wifi_config_t sta = {0};
    strlcpy((char*)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    if (psk && psk[0]) strlcpy((char*)sta.sta.password, psk, sizeof(sta.sta.password));
    sta.sta.scan_method = WIFI_FAST_SCAN;
    sta.sta.bssid_set = false;
    sta.sta.channel = 0; // auto
    sta.sta.listen_interval = 0;
    // NOTE: Hidden SSID handling: there is no ssid_hidden field for station config in ESP-IDF.
    // For hidden networks we may need an active scan with show_hidden=true (handled separately
    // in scan path) or provide channel/BSSID if known for faster connect. Placeholder only.
    (void)hidden;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    ESP_LOGI(TAG, "connect STA (hidden=%d)", hidden);
    return esp_wifi_connect();
}

// Trigger scan (active). Caller reads results via esp_wifi_scan_get_ap_records.
__attribute__((weak))
esp_err_t opdi_net_hosted_scan(void) {
    wifi_scan_config_t cfg = {0};
    cfg.show_hidden = false;
    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    return esp_wifi_scan_start(&cfg, false);
}

