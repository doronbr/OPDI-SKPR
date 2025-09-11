// IR policy scaffolding - purely logic layer (no hardware toggling yet)
#include "opdi_cam.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "opdi_api_ws.h"

static const char *TAG = "opdi_cam_ir";
static opdi_ir_mode_t s_mode = OPDI_IR_MODE_AUTO;
static bool s_active = false; // actual GPIO state (logical ON)
static uint16_t s_y_low=40, s_y_high=60; // thresholds
static uint16_t s_on_ms=3000, s_off_ms=3000;
static uint64_t s_state_change_deadline = 0; // microsecond timestamp for hysteresis gate
static bool s_waiting_on = false; // hysteresis direction flags
static bool s_waiting_off = false;
static bool s_gpio_inited = false;

static void apply_gpio(void){
    if (!s_gpio_inited){
        gpio_config_t io = {0};
        io.pin_bit_mask = 1ULL << CONFIG_OPDI_IR_GPIO;
        io.mode = GPIO_MODE_OUTPUT;
        gpio_config(&io);
        s_gpio_inited = true;
    }
    // Active-low drive
    gpio_set_level(CONFIG_OPDI_IR_GPIO, s_active ? 0 : 1);
}

esp_err_t opdi_cam_ir_set_mode(opdi_ir_mode_t mode){
    s_mode = mode;
    s_waiting_on = s_waiting_off = false;
    if (mode == OPDI_IR_MODE_ON){ s_active = true; }
    else if (mode == OPDI_IR_MODE_OFF){ s_active = false; }
    ESP_LOGI(TAG, "ir mode set %d active=%d", (int)mode, (int)s_active);
    char buf[96]; snprintf(buf,sizeof(buf),"{\"type\":\"cam.ir\",\"mode\":%d,\"active\":%s}",(int)s_mode, s_active?"true":"false");
    opdi_api_ws_broadcast(buf, strlen(buf));
    apply_gpio();
    return ESP_OK;
}

opdi_ir_mode_t opdi_cam_ir_get_mode(void){ return s_mode; }
bool opdi_cam_ir_is_active(void){ return s_active; }

// Internal helper for future: evaluate_auto(y_avg) - not yet wired
// Hysteresis evaluation called from telemetry frame hook
static void evaluate_auto(uint16_t y_avg){
    if (s_mode != OPDI_IR_MODE_AUTO) return;
    uint64_t now_us = esp_timer_get_time();
    if (!s_active){
        if (y_avg < s_y_low){
            if (!s_waiting_on){ s_waiting_on = true; s_state_change_deadline = now_us + (uint64_t)s_on_ms * 1000ULL; }
            else if (now_us >= s_state_change_deadline){ s_active = true; s_waiting_on = false; apply_gpio(); ESP_LOGI(TAG, "IR -> ON (y=%u)", y_avg); char jb[96]; snprintf(jb,sizeof(jb),"{\"type\":\"cam.ir\",\"mode\":%d,\"active\":true}",(int)s_mode); opdi_api_ws_broadcast(jb, strlen(jb)); }
        } else {
            s_waiting_on = false;
        }
    } else { // currently active
        if (y_avg > s_y_high){
            if (!s_waiting_off){ s_waiting_off = true; s_state_change_deadline = now_us + (uint64_t)s_off_ms * 1000ULL; }
            else if (now_us >= s_state_change_deadline){ s_active = false; s_waiting_off = false; apply_gpio(); ESP_LOGI(TAG, "IR -> OFF (y=%u)", y_avg); char jb[96]; snprintf(jb,sizeof(jb),"{\"type\":\"cam.ir\",\"mode\":%d,\"active\":false}",(int)s_mode); opdi_api_ws_broadcast(jb, strlen(jb)); }
        } else {
            s_waiting_off = false;
        }
    }
}

// Exposed weak symbol used by telemetry module to feed luminance
void opdi_cam_ir_eval(uint16_t y_avg){ evaluate_auto(y_avg); }
