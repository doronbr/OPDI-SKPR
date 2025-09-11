// Camera REST endpoints (logic-layer scaffold)
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "opdi_cam.h"
#include <stdio.h>

static const char *TAG = "routes_cam";

static const char *profile_str(opdi_cam_profile_t p){
    switch(p){
        case OPDI_CAM_PROFILE_720P: return "720p";
        case OPDI_CAM_PROFILE_240P: return "240p";
        default: return "480p";
    }
}
static const char *state_str(opdi_cam_state_t s){
    switch(s){
        case OPDI_CAM_STATE_IDLE: return "IDLE";
        case OPDI_CAM_STATE_PREVIEW: return "PREVIEW";
        case OPDI_CAM_STATE_RUN: return "RUN";
        case OPDI_CAM_STATE_FAULT: return "FAULT";
        default: return "INIT";
    }
}

static esp_err_t cam_info_get(httpd_req_t *req){
    opdi_cam_telemetry_t t; opdi_cam_get_telemetry(&t);
    char buf[320];
    int n = snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\",\"profile\":\"%s\",\"fps_target\":%u,\"fps_capture\":%u,\"fps_stream\":%u,\"jpeg_q\":%u,\"luma_avg\":%u,\"ir\":{\"mode\":%u,\"active\":%s}}",
        state_str(opdi_cam_manager_get_state()), profile_str(t.active_profile), t.fps_target, t.fps_capture, t.fps_stream,
        t.jpeg_q_current, t.luma_avg, (unsigned)t.ir_mode_cfg, t.ir_active?"true":"false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

static esp_err_t cam_config_get(httpd_req_t *req){
    opdi_cam_ext_config_t c; opdi_cam_ext_config_get(&c);
    char buf[384];
    int n = snprintf(buf, sizeof(buf),
        "{\"version\":%u,\"profile\":\"%s\",\"fps\":%u,\"jpeg_q\":%u,\"ae_lock\":%s,\"exposure_us\":%u,\"agc_gain\":%u,\"wb\":%u,\"flip\":%s,\"mirror\":%s,\"ir_mode\":%u,\"ir_thresh\":{\"y_low\":%u,\"y_high\":%u,\"h_on\":%u,\"h_off\":%u}}",
        c.version, profile_str(c.profile), c.fps_target, c.jpeg_q, c.ae_lock?"true":"false", c.exposure_us, c.agc_gain, c.wb_mode,
        c.flip?"true":"false", c.mirror?"true":"false", c.ir_mode, c.ir_y_low, c.ir_y_high, c.ir_hyst_on_ms, c.ir_hyst_off_ms);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

static esp_err_t cam_config_put(httpd_req_t *req){
    char content[512]; int received = httpd_req_recv(req, content, sizeof(content)-1);
    if (received <= 0){ httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_OK; }
    content[received]='\0';
    cJSON *root = cJSON_Parse(content);
    if (!root){ httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json"); return ESP_OK; }
    opdi_cam_ext_config_t c; opdi_cam_ext_config_get(&c);
    cJSON *j_prof = cJSON_GetObjectItemCaseSensitive(root, "profile");
    cJSON *j_fps = cJSON_GetObjectItemCaseSensitive(root, "fps");
    cJSON *j_q = cJSON_GetObjectItemCaseSensitive(root, "jpeg_q");
    cJSON *j_ae = cJSON_GetObjectItemCaseSensitive(root, "ae_lock");
    cJSON *j_exp = cJSON_GetObjectItemCaseSensitive(root, "exposure_us");
    cJSON *j_gain = cJSON_GetObjectItemCaseSensitive(root, "agc_gain");
    cJSON *j_flip = cJSON_GetObjectItemCaseSensitive(root, "flip");
    cJSON *j_mirror = cJSON_GetObjectItemCaseSensitive(root, "mirror");
    cJSON *j_ir_mode = cJSON_GetObjectItemCaseSensitive(root, "ir_mode");
    cJSON *j_ir_thresh = cJSON_GetObjectItemCaseSensitive(root, "ir_thresh");
    if (cJSON_IsString(j_prof)){
        if (strcmp(j_prof->valuestring, "720p")==0) c.profile = OPDI_CAM_PROFILE_720P;
        else if (strcmp(j_prof->valuestring, "240p")==0) c.profile = OPDI_CAM_PROFILE_240P;
        else c.profile = OPDI_CAM_PROFILE_480P;
    }
    if (cJSON_IsNumber(j_fps)) c.fps_target = (uint8_t)j_fps->valuedouble;
    if (cJSON_IsNumber(j_q)) c.jpeg_q = (uint8_t)j_q->valuedouble;
    if (cJSON_IsBool(j_ae)) c.ae_lock = cJSON_IsTrue(j_ae);
    if (cJSON_IsNumber(j_exp)) c.exposure_us = (uint32_t)j_exp->valuedouble;
    if (cJSON_IsNumber(j_gain)) c.agc_gain = (uint16_t)j_gain->valuedouble;
    if (cJSON_IsBool(j_flip)) c.flip = cJSON_IsTrue(j_flip);
    if (cJSON_IsBool(j_mirror)) c.mirror = cJSON_IsTrue(j_mirror);
    if (cJSON_IsString(j_ir_mode)){
        if (strcmp(j_ir_mode->valuestring, "on")==0) c.ir_mode = OPDI_IR_MODE_ON;
        else if (strcmp(j_ir_mode->valuestring, "off")==0) c.ir_mode = OPDI_IR_MODE_OFF;
        else c.ir_mode = OPDI_IR_MODE_AUTO;
    }
    if (cJSON_IsObject(j_ir_thresh)){
        cJSON *jl = cJSON_GetObjectItemCaseSensitive(j_ir_thresh, "y_low");
        cJSON *jh = cJSON_GetObjectItemCaseSensitive(j_ir_thresh, "y_high");
        cJSON *jon = cJSON_GetObjectItemCaseSensitive(j_ir_thresh, "hyst_on_ms");
        cJSON *joff = cJSON_GetObjectItemCaseSensitive(j_ir_thresh, "hyst_off_ms");
        if (cJSON_IsNumber(jl)) c.ir_y_low = (uint16_t)jl->valuedouble;
        if (cJSON_IsNumber(jh)) c.ir_y_high = (uint16_t)jh->valuedouble;
        if (cJSON_IsNumber(jon)) c.ir_hyst_on_ms = (uint16_t)jon->valuedouble;
        if (cJSON_IsNumber(joff)) c.ir_hyst_off_ms = (uint16_t)joff->valuedouble;
    }
    cJSON_Delete(root);
    opdi_cam_ext_config_set(&c);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t cam_start_post(httpd_req_t *req){ opdi_cam_manager_start(); httpd_resp_sendstr(req, "{\"ok\":true}"); return ESP_OK; }
static esp_err_t cam_stop_post(httpd_req_t *req){ opdi_cam_manager_stop(); httpd_resp_sendstr(req, "{\"ok\":true}"); return ESP_OK; }
static esp_err_t cam_detect_post(httpd_req_t *req){ opdi_cam_manager_set_detection(true); httpd_resp_sendstr(req, "{\"ok\":true}"); return ESP_OK; }
static esp_err_t cam_detect_off_post(httpd_req_t *req){ opdi_cam_manager_set_detection(false); httpd_resp_sendstr(req, "{\"ok\":true}"); return ESP_OK; }

void routes_camera_register(httpd_handle_t server){
    const httpd_uri_t endpoints[] = {
        { .uri="/api/v1/camera/info",   .method=HTTP_GET,  .handler=cam_info_get },
        { .uri="/api/v1/camera/config", .method=HTTP_GET,  .handler=cam_config_get },
        { .uri="/api/v1/camera/config", .method=HTTP_PUT,  .handler=cam_config_put },
        { .uri="/api/v1/camera/start",  .method=HTTP_POST, .handler=cam_start_post },
    { .uri="/api/v1/camera/stop",   .method=HTTP_POST, .handler=cam_stop_post },
    { .uri="/api/v1/camera/detect/on",  .method=HTTP_POST, .handler=cam_detect_post },
    { .uri="/api/v1/camera/detect/off", .method=HTTP_POST, .handler=cam_detect_off_post },
    };
    for (size_t i=0;i<sizeof(endpoints)/sizeof(endpoints[0]);++i) httpd_register_uri_handler(server, &endpoints[i]);
    ESP_LOGI(TAG, "camera routes registered");
}

// MJPEG streaming endpoint (poll-based; basic implementation)
static esp_err_t cam_stream_get(httpd_req_t *req){
    static const char *BOUNDARY = "frame";
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
    char header[128];
    while(1){
        // Acquire latest frame (copy). If none yet, delay.
        int need = opdi_cam_stream_copy_latest(NULL, 0, NULL, NULL, NULL);
        if (need <= 0){ vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        if (need > 64*1024){ // crude cap; prevent huge allocations
            vTaskDelay(pdMS_TO_TICKS(100)); continue;
        }
        uint8_t *tmp = malloc(need);
        if (!tmp){ vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        uint8_t q=0; opdi_cam_profile_t prof=0; uint32_t ts=0;
        int got = opdi_cam_stream_copy_latest(tmp, need, &q, &prof, &ts);
        if (got != need || got<=0){ free(tmp); vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        int hn = snprintf(header, sizeof(header), "--%s\r\nContent-Type: image/jpeg\r\nX-Profile: %d\r\nX-JPEG-Q: %u\r\nX-Timestamp: %u\r\nContent-Length: %d\r\n\r\n", BOUNDARY, (int)prof, q, ts, got);
        if (httpd_resp_send_chunk(req, header, hn)!=ESP_OK || httpd_resp_send_chunk(req, (const char*)tmp, got)!=ESP_OK || httpd_resp_send_chunk(req, "\r\n", 2)!=ESP_OK){
            free(tmp); break; // client gone
        }
        free(tmp);
        vTaskDelay(pdMS_TO_TICKS(50)); // ~20 FPS cap
    }
    return ESP_OK;
}

void routes_camera_register_stream(httpd_handle_t server){
    httpd_uri_t u_stream = { .uri="/stream", .method=HTTP_GET, .handler=cam_stream_get };
    httpd_register_uri_handler(server, &u_stream);
}
