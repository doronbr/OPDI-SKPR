param()

$ErrorActionPreference = "Stop"

function Ensure-Dir($p) {
  if (-not (Test-Path $p)) { New-Item -ItemType Directory -Force -Path $p | Out-Null }
}

function Write-File($path, $content) {
  Ensure-Dir (Split-Path -Parent $path)
  Set-Content -Path $path -Value $content -Encoding UTF8 -NoNewline:$false
}

function Ensure-Line($file, $line) {
  if (-not (Test-Path $file)) { New-Item -ItemType File -Path $file | Out-Null }
  $cur = Get-Content $file -Raw
  if ($cur -notmatch [regex]::Escape($line)) {
    Add-Content -Path $file -Value $line
  }
}

# --- Files to add/update ---

# components/opdi_net/CMakeLists.txt
$cmake = @"
idf_component_register(
  SRCS "opdi_net.c" "opdi_net_hosted.c"
  INCLUDE_DIRS "include"
  REQUIRES esp_wifi nvs_flash esp_event lwip
)
"@
Write-File "components/opdi_net/CMakeLists.txt" $cmake

# components/opdi_net/Kconfig
$kconfig = @"
menu "OPDI Networking (ESP-Hosted)"

config OPDI_HOSTED_BUS
    string "ESP-Hosted transport"
    default "spi"

config OPDI_COUNTRY_CODE
    string "Wi-Fi country code"
    default "IL"

config OPDI_NET_MAX_PROFILES
    int "Max saved SSIDs"
    default 20
    range 1 20

config OPDI_NET_STA_TIMEOUT_S
    int "Per-SSID connect timeout (s)"
    default 60
    range 10 300

config OPDI_NET_RETRIES
    int "Total STA retries before AP"
    default 20
    range 1 100

config OPDI_NET_RETRY_PERIOD_S
    int "STA re-attempt in AP (s)"
    default 120
    range 30 600

config OPDI_NET_SCAN_MIN_RSSI
    int "Ignore APs weaker than (dBm)"
    default -90
    range -100 -30

config OPDI_AP_CHANNEL
    int "AP channel"
    default 1
    range 1 13

config OPDI_AP_OPEN
    bool "Provisioning AP is open"
    default y

config OPDI_DHCP_HOSTNAME
    string "DHCP hostname"
    default "opdi-skpr"

endmenu
"@
Write-File "components/opdi_net/Kconfig" $kconfig

# components/opdi_net/include/opdi_net.h
$hdr = @"
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

#ifdef __cplusplus
}
#endif
"@
Write-File "components/opdi_net/include/opdi_net.h" $hdr

# components/opdi_net/opdi_net.c (state-machine stub)
$src = @"
#include "opdi_net.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_event.h"

static const char *TAG = "opdi_net";
static opdi_net_state_t g_state = NET_INIT;

esp_err_t opdi_net_init(void) {
    ESP_LOGI(TAG, "init (ESP-Hosted, country=%s)", CONFIG_OPDI_COUNTRY_CODE);
    // TODO: init ESP-Hosted link, load NVS profiles, set country, register events
    g_state = NET_STA_CONNECT;
    return ESP_OK;
}

esp_err_t opdi_net_add_profile(const opdi_net_profile_t *p) {
    // TODO: save to NVS (MRU), validate lengths and auth
    ESP_LOGI(TAG, "add profile ssid='%s'%s", p->ssid, p->hidden ? " (hidden)" : "");
    return ESP_OK;
}

esp_err_t opdi_net_forget(const char *ssid) {
    // TODO: remove from NVS and MRU list
    ESP_LOGI(TAG, "forget ssid='%s'", ssid);
    return ESP_OK;
}

esp_err_t opdi_net_connect(const char *ssid_or_null) {
    // TODO: drive connect attempt (direct if hidden or MRU flow)
    g_state = NET_STA_CONNECT;
    ESP_LOGI(TAG, "connect: %s", ssid_or_null ? ssid_or_null : "(MRU)");
    return ESP_OK;
}

opdi_net_state_t opdi_net_get_state(void) { return g_state; }
"@
Write-File "components/opdi_net/opdi_net.c" $src

# components/opdi_net/opdi_net_hosted.c (bring-up stub)
$hosted = @"
#include "esp_log.h"

static const char *TAG = "opdi_net_hosted";

// Weak symbol; replace when wiring ESP-Hosted host library
__attribute__((weak))
void opdi_net_hosted_start(void) {
    // TODO: init ESP-Hosted bus (SPI/SDIO/UART), bring up NWP, create netif
    ESP_LOGI(TAG, "ESP-Hosted link start (bus=%s)", CONFIG_OPDI_HOSTED_BUS);
}
"@
Write-File "components/opdi_net/opdi_net_hosted.c" $hosted

# main/routes_net.c (REST status endpoint)
$routes = @"
#include <stdio.h>
#include "esp_http_server.h"
#include "opdi_net.h"

static esp_err_t net_status_get(httpd_req_t *req){
    const char *state =
        (opdi_net_get_state()==NET_STA_CONNECTED) ? "STA_CONNECTED" :
        (opdi_net_get_state()==NET_AP_ACTIVE)     ? "AP_ACTIVE" :
        (opdi_net_get_state()==NET_STA_CONNECT)   ? "STA_CONNECT" : "INIT";

    char buf[96];
    int n = snprintf(buf, sizeof(buf), "{\"state\":\"%s\"}", state);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

void routes_net_register(httpd_handle_t server){
    httpd_uri_t u = {
        .uri = "/api/v1/net/sta/status",
        .method = HTTP_GET,
        .handler = net_status_get,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &u);
}
"@
Write-File "main/routes_net.c" $routes

# docs/networking.md (skeleton)
$doc = @"
# Networking (ESP-Hosted)

- **Host:** ESP32-P4 runs application + lwIP.
- **NWP:** ESP32-C6 runs Wi-Fi stack (ESP-Hosted over SPI by default).
- **Region:** IL (Israel).
- **Provisioning AP:** SSID `OPDI_SKPR-XXXX` (open), channel configurable.
- **Policy:** MRU order (20 profiles max), hidden SSIDs supported, periodic re-attempts in AP mode.

## States
INIT  STA_CONNECT  (STA_CONNECTED | AP_ACTIVE).  
AP_ACTIVE performs periodic scans and tries STA every `CONFIG_OPDI_NET_RETRY_PERIOD_S`.

## Key Kconfig
- `CONFIG_OPDI_COUNTRY_CODE="IL"`
- `CONFIG_OPDI_NET_MAX_PROFILES=20`
- `CONFIG_OPDI_NET_RETRIES=20`
- `CONFIG_OPDI_AP_OPEN=y`
- `CONFIG_OPDI_NET_STA_TIMEOUT_S=60`
- `CONFIG_OPDI_NET_RETRY_PERIOD_S=120`
- `CONFIG_OPDI_NET_SCAN_MIN_RSSI=-90`
- `CONFIG_OPDI_AP_CHANNEL=1`

## REST
- `GET /api/v1/net/sta/status`  `{ state }`

## TODO
- Wire ESP-Hosted host library and implement scan/connect/AP wrappers.
- Add profile CRUD endpoints and NVS schema.
"@
Write-File "docs/networking.md" $doc

# sdkconfig.defaults: ensure required defaults (append if missing)
$defs = @(
  'CONFIG_OPDI_HOSTED_BUS="spi"',
  'CONFIG_OPDI_COUNTRY_CODE="IL"',
  'CONFIG_OPDI_NET_MAX_PROFILES=20',
  'CONFIG_OPDI_NET_STA_TIMEOUT_S=60',
  'CONFIG_OPDI_NET_RETRIES=20',
  'CONFIG_OPDI_NET_RETRY_PERIOD_S=120',
  'CONFIG_OPDI_NET_SCAN_MIN_RSSI=-90',
  'CONFIG_OPDI_AP_CHANNEL=1',
  'CONFIG_OPDI_AP_OPEN=y',
  'CONFIG_OPDI_DHCP_HOSTNAME="opdi-skpr"'
)
foreach ($d in $defs) { Ensure-Line "sdkconfig.defaults" $d }

# Optional: sanity check CI (do not overwrite existing workflow)
$wf = ".github/workflows/build.yml"
if (Test-Path $wf) {
  $w = Get-Content $wf -Raw
  if ($w -notmatch "espressif/esp-idf-ci-action") {
    Write-Host "NOTE: Consider running idf.py inside espressif/esp-idf-ci-action@v1 for reliability."
  }
}

Write-Host "Staging files..."
git add components/opdi_net docs/networking.md main/routes_net.c sdkconfig.defaults

Write-Host "Committing..."
git commit -m "net: add ESP-Hosted skeleton, REST status endpoint, and Wi-Fi defaults (IL, open AP, NET_RETRIES=20)"

Write-Host "Done."
