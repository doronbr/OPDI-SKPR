#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { NET_INIT, NET_STA_CONNECT, NET_STA_CONNECTED, NET_AP_ACTIVE } opdi_net_state_t;

typedef struct {
    char ssid[33];
    char psk[65];
    uint8_t auth;   // WIFI_AUTH_* enum
    bool hidden;
} opdi_net_profile_t;

esp_err_t opdi_net_init(void);
esp_err_t opdi_net_add_profile(const opdi_net_profile_t *p);
esp_err_t opdi_net_forget(const char *ssid);
esp_err_t opdi_net_connect(const char *ssid_or_null);
opdi_net_state_t opdi_net_get_state(void);

// AP config (persisted) - simple setter/getter for fallback AP mode
esp_err_t opdi_net_ap_set(const char *ssid, uint8_t channel);
void opdi_net_ap_get(char *out_ssid, size_t len, uint8_t *out_channel);

// Metrics snapshot (Phase1 basic counters)
typedef struct {
    uint32_t connect_attempts;      // total connection attempts since boot
    uint32_t connects_success;      // successful connections (got IP)
    uint32_t avg_connect_time_ms;   // average time between attempt start and IP acquisition
    uint32_t scan_count;            // number of Wi-Fi scans performed
    uint32_t current_retries;       // retries since last successful connection
} opdi_net_metrics_t;

void opdi_net_get_metrics(opdi_net_metrics_t *out);
// Cached IP/GW accessors (may return empty string if not connected)
const char *opdi_net_ip_cached(void);
const char *opdi_net_gw_cached(void);
// Hosted firmware version (may be empty if not available)
const char *opdi_net_hosted_fw_version(void);

// Serialize recent log events (JSON array) into provided buffer; returns length (0 if insufficient)
size_t opdi_net_logs_serialize(char *out, size_t cap);

#ifdef CONFIG_OPDI_NET_TESTING
void opdi_net_test_force_state(opdi_net_state_t st);
void opdi_net_test_simulate_sta_connected(const char *ip, const char *gw, int rssi);
#endif

#ifdef __cplusplus
}
#endif
