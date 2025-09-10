#!/usr/bin/env bash
set -euo pipefail

root="$(pwd)"
mkdir -p "$root"/{main,components,docs,docs/api,.github/workflows,tools}

# --- Core project files ---
cat > CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "0.1.0")
project(opdi_skpr)
set(SDKCONFIG_DEFAULTS ${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults)
EOF

cat > sdkconfig.defaults <<'EOF'
# Network & HTTP server
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024
CONFIG_HTTPD_WS_SUPPORT=y
# Hostname (SRD §4)
CONFIG_LWIP_LOCAL_HOSTNAME="opdi-skpr"
EOF

cat > partitions.csv <<'EOF'
# Name, Type, SubType, Offset, Size, Flags
nvs,data,nvs,0x9000,24K,
phy_init,data,phy,0xF000,4K,
factory,app,factory,0x10000,4M,
ota_0,app,ota_0,,4M,
ota_1,app,ota_1,,4M,
storage,data,fat,,16M,
coredump,data,coredump,,1M,
EOF

cat > idf_component.yml <<'EOF'
dependencies:
  espressif/esp-idf: "==5.5"
  espressif/esp_http_server: "^1.3.1"
EOF

# --- Minimal app that builds and exposes / and /api/v1/system/info ---
cat > main/CMakeLists.txt <<'EOF'
idf_component_register(SRCS "app_main.c" INCLUDE_DIRS ".")
EOF

cat > main/app_main.c <<'EOF'
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_http_server.h"

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
  return h;
}
void app_main(void) {
  ESP_ERROR_CHECK(nvs_flash_init());
  start_httpd();
  ESP_LOGI(TAG, "OPDI_SKPR up");
  while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
EOF

# --- Docs placeholders (you’ll paste the full SRD from canvas) ---
mkdir -p docs/api
cat > docs/README.md <<'EOF'
# OPDI_SKPR Docs
- See **docs/SRD.md** for the Software Requirements Document.
- API schema: **docs/api/openapi.yaml** (Draft; see SRD Appendix I2).
EOF

cat > docs/SRD.md <<'EOF'
# OPDI_SKPR – Software Requirements Document (SRD)
(Placeholder)
→ Copy the full SRD content from the project canvas into this file.
EOF

cat > docs/api/openapi.yaml <<'EOF'
openapi: 3.0.3
info: { title: OPDI_SKPR Device API, version: 0.1.0 }
paths:
  /api/v1/system/info:
    get: { summary: Get device info, responses: { '200': { description: OK } } }
# Replace with full spec from SRD Appendix I2
EOF

# --- CI workflow (build on GitHub) ---
mkdir -p .github/workflows
cat > .github/workflows/build.yml <<'EOF'
name: build
on:
  push:
  pull_request:
jobs:
  esp32p4:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: espressif/esp-idf-ci-action@v1
        with:
          esp_idf_version: v5.5
      - name: Set target
        run: idf.py set-target esp32p4
      - name: Build
        run: idf.py build
      - name: Size report
        run: idf.py size-components
      - name: Artifacts
        uses: actions/upload-artifact@v4
        with:
          name: firmware
          path: build/*.bin
EOF

# --- Repo hygiene ---
cat > .gitignore <<'EOF'
build/
sdkconfig
sdkconfig.old
.vscode/
*.pyc
__pycache__/
EOF

echo "Bootstrapped. Next:"
echo "1) git add -A && git commit -m 'chore: bootstrap ESP-IDF project + CI'"
echo "2) git push origin HEAD"
