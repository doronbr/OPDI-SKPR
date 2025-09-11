// Camera manager logic layer scaffolding (no duplication of low-level drivers)
#include "opdi_cam.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "opdi_api_ws.h"

static const char *TAG = "opdi_cam_mgr";

#define OPDI_CAM_NVS_NS  "camera"
#define OPDI_CAM_NVS_KEY "ext_cfg"

static opdi_cam_ext_config_t s_ext_cfg; // persisted
static bool s_ext_loaded = false;
static opdi_cam_state_t s_state = OPDI_CAM_STATE_INIT;
static bool s_detection_enabled = false;
// Provide weak setter hook to telemetry (implemented there later if needed)
__attribute__((weak)) void opdi_cam_telemetry_seed(opdi_cam_profile_t prof, uint8_t jpeg_q, uint8_t fps_target, opdi_ir_mode_t ir_mode){
    (void)prof; (void)jpeg_q; (void)fps_target; (void)ir_mode; /* no-op until telemetry exposes internal setter */
}

// Forward declarations (internal helpers)
static void cam_ext_config_apply_runtime(const opdi_cam_ext_config_t *c);
static void cam_ext_config_load_defaults(opdi_cam_ext_config_t *c){
    memset(c, 0, sizeof(*c));
    c->version = OPDI_CAM_EXT_CONFIG_VERSION;
    // Map string profile default
    const char *dp = CONFIG_OPDI_CAM_DEFAULT_PROFILE;
    if (dp && strcmp(dp, "720p") == 0) c->profile = OPDI_CAM_PROFILE_720P;
    else if (dp && strcmp(dp, "240p") == 0) c->profile = OPDI_CAM_PROFILE_240P;
    else c->profile = OPDI_CAM_PROFILE_480P;
    c->fps_target = CONFIG_OPDI_CAM_DEFAULT_FPS;
    c->jpeg_q = CONFIG_OPDI_CAM_DEFAULT_JPEG_Q;
    c->wb_mode = OPDI_CAM_WB_AUTO;
    c->bcsh_brightness = 0; c->bcsh_contrast = 0; c->bcsh_saturation = 0; c->bcsh_sharpness = 0;
    c->ir_mode = (strcmp(CONFIG_OPDI_IR_DEFAULT_MODE, "on")==0)?OPDI_IR_MODE_ON:
                 (strcmp(CONFIG_OPDI_IR_DEFAULT_MODE, "off")==0)?OPDI_IR_MODE_OFF:OPDI_IR_MODE_AUTO;
    c->ir_y_low = CONFIG_OPDI_IR_Y_LOW;
    c->ir_y_high = CONFIG_OPDI_IR_Y_HIGH;
    c->ir_hyst_on_ms = CONFIG_OPDI_IR_HYST_ON_MS;
    c->ir_hyst_off_ms = CONFIG_OPDI_IR_HYST_OFF_MS;
}

static void clamp_ext_config(opdi_cam_ext_config_t *c){
    if (!c) return;
    if (c->fps_target < 10) c->fps_target = 10; else if (c->fps_target > 30) c->fps_target = 30;
    if (c->jpeg_q < 50) c->jpeg_q = 50; else if (c->jpeg_q > 90) c->jpeg_q = 90;
    if (c->bcsh_brightness < -2) c->bcsh_brightness=-2; else if (c->bcsh_brightness>2) c->bcsh_brightness=2;
    if (c->bcsh_contrast   < -2) c->bcsh_contrast=-2;   else if (c->bcsh_contrast>2) c->bcsh_contrast=2;
    if (c->bcsh_saturation < -2) c->bcsh_saturation=-2; else if (c->bcsh_saturation>2) c->bcsh_saturation=2;
    if (c->bcsh_sharpness  < -2) c->bcsh_sharpness=-2;  else if (c->bcsh_sharpness>2) c->bcsh_sharpness=2;
    if (c->ir_y_low > c->ir_y_high) { uint16_t t = c->ir_y_low; c->ir_y_low = c->ir_y_high; c->ir_y_high = t; }
}

static esp_err_t persist_ext_config(const opdi_cam_ext_config_t *c){
    nvs_handle_t h; esp_err_t e = nvs_open(OPDI_CAM_NVS_NS, NVS_READWRITE, &h);
    if (e!=ESP_OK) return e;
    e = nvs_set_blob(h, OPDI_CAM_NVS_KEY, c, sizeof(*c));
    if (e==ESP_OK) e = nvs_commit(h);
    nvs_close(h); return e;
}

static void cam_ext_config_apply_runtime(const opdi_cam_ext_config_t *c){
    (void)c; // Placeholder: actual application to sensor/ISP delegated to existing components later.
}

// --------------- WebSocket event helpers ---------------
static void cam_ws_emit(const char *json_body){
    if (!json_body) return;
    opdi_api_ws_broadcast(json_body, strlen(json_body));
}
static void cam_ws_emit_state(opdi_cam_state_t st){
    char buf[96]; snprintf(buf,sizeof(buf),"{\"type\":\"cam.state\",\"state\":%d}",(int)st); cam_ws_emit(buf);
}
static void cam_ws_emit_ir(bool active, opdi_ir_mode_t mode){
    char buf[112]; snprintf(buf,sizeof(buf),"{\"type\":\"cam.ir\",\"mode\":%d,\"active\":%s}",(int)mode, active?"true":"false"); cam_ws_emit(buf);
}

// --------------- Capture / streaming task ---------------
static TaskHandle_t s_stream_task = NULL;
static void cam_stream_task(void *arg){
    (void)arg;
    // Simple loop: use snapshot API as provisional frame source until direct pipeline integration.
    // Obtains target FPS from current config each cycle.
    const TickType_t min_delay = pdMS_TO_TICKS(5);
    while(1){
        if (s_state != OPDI_CAM_STATE_PREVIEW && s_state != OPDI_CAM_STATE_RUN){ vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        opdi_cam_ext_config_t c; opdi_cam_ext_config_get(&c);
        uint32_t interval_ms = c.fps_target ? (1000U / c.fps_target) : 100;
        int need = opdi_cam_snapshot(NULL, 0);
        if (need > 0 && need < 400000){
            uint8_t *buf = (uint8_t*)malloc(need);
            if (buf){
                int got = opdi_cam_snapshot(buf, need);
                if (got == need){
                    // Placeholder luminance estimate: use mid value if accessible; fall back constant.
                    uint16_t y = 50;
                    if (need > 20){ y = (uint16_t)(buf[20]); }
                    opdi_cam_on_frame(y);
                    opdi_cam_stream_push_jpeg(buf, (size_t)got, c.profile, c.jpeg_q);
                }
                free(buf);
            }
        }
        TickType_t delay_ticks = pdMS_TO_TICKS(interval_ms);
        if (delay_ticks < min_delay) delay_ticks = min_delay;
        vTaskDelay(delay_ticks);
    }
}

static void ensure_stream_task_started(void){
    if (!s_stream_task){
        xTaskCreate(cam_stream_task, "cam_stream", 4096, NULL, 5, &s_stream_task);
    }
}

esp_err_t opdi_cam_manager_init(void){
    if (s_ext_loaded) return ESP_OK;
    nvs_handle_t h; esp_err_t e = nvs_open(OPDI_CAM_NVS_NS, NVS_READWRITE, &h);
    if (e==ESP_OK){
        size_t sz = sizeof(s_ext_cfg);
        esp_err_t r = nvs_get_blob(h, OPDI_CAM_NVS_KEY, &s_ext_cfg, &sz);
        if (r==ESP_OK && sz==sizeof(s_ext_cfg) && s_ext_cfg.version==OPDI_CAM_EXT_CONFIG_VERSION){
            clamp_ext_config(&s_ext_cfg); s_ext_loaded=true; ESP_LOGI(TAG, "ext config loaded v%u", s_ext_cfg.version);
        } else {
            cam_ext_config_load_defaults(&s_ext_cfg); persist_ext_config(&s_ext_cfg); s_ext_loaded=true; ESP_LOGI(TAG, "ext config defaults applied");
        }
        nvs_close(h);
    } else {
        cam_ext_config_load_defaults(&s_ext_cfg); s_ext_loaded=true; ESP_LOGW(TAG, "nvs open fail (%s), using defaults", esp_err_to_name(e));
    }
    cam_ext_config_apply_runtime(&s_ext_cfg);
    // Initialize telemetry base fields
    opdi_cam_telemetry_seed(s_ext_cfg.profile, s_ext_cfg.jpeg_q, s_ext_cfg.fps_target, s_ext_cfg.ir_mode);
    s_state = OPDI_CAM_STATE_IDLE; // Underlying low-level init assumed done earlier via opdi_cam_init()
    ensure_stream_task_started();
    cam_ws_emit_state(s_state);
    return ESP_OK;
}

esp_err_t opdi_cam_manager_start(void){
    if (s_state == OPDI_CAM_STATE_PREVIEW || s_state == OPDI_CAM_STATE_RUN) return ESP_OK;
    if (s_state == OPDI_CAM_STATE_FAULT) return ESP_FAIL;
    s_state = OPDI_CAM_STATE_PREVIEW; // Placeholder: start streaming later
    ESP_LOGI(TAG, "state -> PREVIEW");
    cam_ws_emit_state(s_state);
    return ESP_OK;
}

esp_err_t opdi_cam_manager_stop(void){
    if (s_state == OPDI_CAM_STATE_IDLE) return ESP_OK;
    if (s_state == OPDI_CAM_STATE_FAULT) return ESP_FAIL;
    s_detection_enabled = false;
    s_state = OPDI_CAM_STATE_IDLE;
    ESP_LOGI(TAG, "state -> IDLE");
    cam_ws_emit_state(s_state);
    return ESP_OK;
}

esp_err_t opdi_cam_manager_set_detection(bool enable){
    if (s_state == OPDI_CAM_STATE_FAULT) return ESP_FAIL;
    if (enable){
        if (s_state == OPDI_CAM_STATE_IDLE){ // auto start preview first
            opdi_cam_manager_start();
        }
        s_state = OPDI_CAM_STATE_RUN;
    } else if (s_state == OPDI_CAM_STATE_RUN){
        s_state = OPDI_CAM_STATE_PREVIEW;
    }
    s_detection_enabled = enable;
    ESP_LOGI(TAG, "detection %s (state=%d)", enable?"on":"off", (int)s_state);
    cam_ws_emit_state(s_state);
    return ESP_OK;
}

opdi_cam_state_t opdi_cam_manager_get_state(void){ return s_state; }

esp_err_t opdi_cam_ext_config_get(opdi_cam_ext_config_t *out){ if(!out) return ESP_ERR_INVALID_ARG; *out = s_ext_cfg; return ESP_OK; }

esp_err_t opdi_cam_ext_config_set(const opdi_cam_ext_config_t *in){
    if(!in) return ESP_ERR_INVALID_ARG;
    opdi_cam_ext_config_t tmp = *in; clamp_ext_config(&tmp); tmp.version = OPDI_CAM_EXT_CONFIG_VERSION;
    s_ext_cfg = tmp; persist_ext_config(&s_ext_cfg); cam_ext_config_apply_runtime(&s_ext_cfg);
    return ESP_OK;
}

// Periodic tick placeholder
void opdi_cam_periodic_1s(void){ /* upcoming: fps counters, governor, telemetry emit */ }

// Governor hint placeholder
void opdi_cam_governor_notify_cpu_load(uint8_t pct){ (void)pct; }
