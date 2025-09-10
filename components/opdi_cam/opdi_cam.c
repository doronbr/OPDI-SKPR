// Phase 2A: integrate basic sensor detection and parameter application.
#include "opdi_cam.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_cam_sensor_detect.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_types.h"
#include "esp_sccb_intf.h"
#include "esp_sccb_types.h"
#include "esp_sccb_i2c.h"
#include "bsp/esp32_p4_function_ev_board.h" // for bsp_i2c_get_handle
#include "driver/gpio.h"
#ifdef OPDI_CAM_HAVE_VIDEO
#include "esp_video.h"
#endif

// Simple one-shot capture context
#ifdef OPDI_CAM_HAVE_VIDEO
static video_device_handle_t s_video_dev = NULL; // created lazily
static bool s_video_inited = false;
#endif

static const char *TAG = "opdi_cam";
#define CAM_NVS_NS "cam"
#define CAM_NVS_KEY "cfg"

static opdi_cam_config_t s_cfg = {0,0,0,true};
static bool s_loaded = false;
static bool s_sensor_ready = false;
static esp_cam_sensor_device_t *s_sensor = NULL;
static SemaphoreHandle_t s_lock; // protect sensor ops

// Simple desired default resolution; will choose first JPEG format if available
typedef struct { uint16_t w; uint16_t h; } cam_pref_res_t;
static const cam_pref_res_t s_pref_list[] = {
    {640,480}, {800,600}, {320,240}
};

// Minimal JPEG (SOI, JFIF APP0, 1x1 pixel dummy quant tables omitted, EOI) - still tiny but more parsers accept it
static const unsigned char s_jpeg_stub[] = {
    0xFF,0xD8,              /* SOI */
    0xFF,0xE0, 0x00,0x10,   /* APP0 length=16 */
    'J','F','I','F',0x00,   /* Identifier */
    0x01,0x01,              /* version 1.01 */
    0x00,                   /* units */
    0x00,0x01, 0x00,0x01,   /* Xdensity=1, Ydensity=1 */
    0x00,0x00,              /* no thumbnail */
    0xFF,0xD9               /* EOI */
};

static void cam_cfg_clamp(opdi_cam_config_t *c){
    if(!c) return; 
    if (c->brightness < -10) c->brightness=-10; else if (c->brightness>10) c->brightness=10;
    if (c->contrast < -10) c->contrast=-10; else if (c->contrast>10) c->contrast=10;
    if (c->saturation < -10) c->saturation=-10; else if (c->saturation>10) c->saturation=10;
}

// Helper: perform optional reset/pwdn sequencing
static void cam_hw_sequence(int reset_pin, int pwdn_pin){
    if (reset_pin >= 0){
        gpio_config_t io = {0}; io.pin_bit_mask = 1ULL << reset_pin; io.mode = GPIO_MODE_OUTPUT; io.pull_up_en = 0; io.pull_down_en = 0; io.intr_type = GPIO_INTR_DISABLE; gpio_config(&io);
    }
    if (pwdn_pin >= 0){
        gpio_config_t io = {0}; io.pin_bit_mask = 1ULL << pwdn_pin; io.mode = GPIO_MODE_OUTPUT; io.pull_up_en = 0; io.pull_down_en = 0; io.intr_type = GPIO_INTR_DISABLE; gpio_config(&io);
    }
    if (pwdn_pin >= 0){
        // Most sensors: PWDN high = powered down; drive low to enable
        gpio_set_level(pwdn_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(pwdn_pin, 0);
        ESP_LOGI(TAG, "pwdn toggled (pin=%d)", pwdn_pin);
    }
    if (reset_pin >= 0){
        // Active low reset: pulse low then high
        gpio_set_level(reset_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(reset_pin, 1);
        ESP_LOGI(TAG, "reset pulse done (pin=%d)", reset_pin);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Attempt detection at a given SCCB speed; returns sensor on success
static esp_cam_sensor_device_t *cam_try_detect(i2c_master_bus_handle_t i2c_bus, int reset_pin, int pwdn_pin, int xclk_pin, int32_t xclk_freq, uint32_t scl_speed){
    esp_err_t r; esp_cam_sensor_device_t *found=NULL; esp_sccb_io_handle_t sccb=NULL;
    sccb_i2c_config_t sccb_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x00,
        .scl_speed_hz = (int)scl_speed,
        .addr_bits_width = 16,
        .val_bits_width = 8,
    };
    r = sccb_new_i2c_io(i2c_bus, &sccb_cfg, &sccb);
    if (r!=ESP_OK){ ESP_LOGW(TAG, "sccb_new_i2c_io speed=%u failed (%s)", (unsigned)scl_speed, esp_err_to_name(r)); return NULL; }
    esp_cam_sensor_config_t scfg = {
        .sccb_handle = sccb,
        .reset_pin = (int8_t)reset_pin,
        .pwdn_pin = (int8_t)pwdn_pin,
        .xclk_pin = (int8_t)xclk_pin,
        .xclk_freq_hz = xclk_freq,
        .sensor_port = ESP_CAM_SENSOR_MIPI_CSI,
    };
    extern esp_cam_sensor_detect_fn_t __esp_cam_sensor_detect_fn_array_start;
    extern esp_cam_sensor_detect_fn_t __esp_cam_sensor_detect_fn_array_end;
    esp_cam_sensor_detect_fn_t *it = &__esp_cam_sensor_detect_fn_array_start;
    int attempts=0;
    for (; it < &__esp_cam_sensor_detect_fn_array_end; ++it){
        attempts++;
        ESP_LOGD(TAG, "detect try addr=0x%02x speed=%u", (unsigned)it->sccb_addr, (unsigned)scl_speed);
        esp_cam_sensor_device_t *dev = it->detect(&scfg);
        if (dev){
            ESP_LOGI(TAG, "sensor detected name=%s addr=0x%02x speed=%u (attempt %d)", dev->name, (unsigned)it->sccb_addr, (unsigned)scl_speed, attempts);
            found = dev;
            break;
        }
    }
    if (!found){
        ESP_LOGW(TAG, "no sensor at speed=%u (attempts=%d)", (unsigned)scl_speed, attempts);
        esp_sccb_del_i2c_io(sccb);
    }
    return found;
}

// Manual OV5647 register probe (returns true if ID matches) using raw SCCB handle.
// OV5647 uses 16-bit register addresses; ID high=0x300A (0x56) low=0x300B (0x47); 7-bit addr typically 0x36.
static bool cam_probe_ov5647(i2c_master_bus_handle_t i2c_bus, uint32_t scl_speed){
    esp_sccb_io_handle_t sccb=NULL; sccb_i2c_config_t cfg={
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x36, // OV5647 7-bit SCCB address
        .scl_speed_hz = (int)scl_speed,
        .addr_bits_width = 16,
        .val_bits_width = 8,
    };
    if (sccb_new_i2c_io(i2c_bus, &cfg, &sccb)!=ESP_OK) return false;
    uint8_t hi=0, lo=0; bool ok=false; esp_err_t r1, r2;
    r1 = esp_sccb_transmit_receive_reg_a16v8(sccb, 0x300A, &hi);
    r2 = esp_sccb_transmit_receive_reg_a16v8(sccb, 0x300B, &lo);
    if (r1==ESP_OK && r2==ESP_OK){
        ESP_LOGI(TAG, "manual probe ov5647 raw id bytes: %02X %02X (speed=%u)", hi, lo, (unsigned)scl_speed);
        if (hi==0x56 && lo==0x47) ok=true; else ESP_LOGW(TAG, "unexpected sensor id bytes (want 56 47)");
    } else {
        ESP_LOGW(TAG, "manual probe ov5647 read failed (r1=%s r2=%s) speed=%u", esp_err_to_name(r1), esp_err_to_name(r2), (unsigned)scl_speed);
    }
    esp_sccb_del_i2c_io(sccb);
    return ok;
}

esp_err_t opdi_cam_init(void){
    if (s_loaded) return ESP_OK;
    nvs_handle_t h; esp_err_t err = nvs_open(CAM_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK){
        size_t req = sizeof(s_cfg);
        opdi_cam_config_t tmp;
        if (nvs_get_blob(h, CAM_NVS_KEY, &tmp, &req)==ESP_OK && req==sizeof(tmp)){
            s_cfg = tmp; cam_cfg_clamp(&s_cfg); s_loaded=true; ESP_LOGI(TAG, "config loaded");
        }
        nvs_close(h);
    }
    if (!s_loaded){ s_loaded=true; ESP_LOGI(TAG, "config default"); }

    // Create lock
    if (!s_lock) s_lock = xSemaphoreCreateMutex();

    // Attempt sensor detection (best effort; keep stub if fails). We will try high speed then fallback to lower speed.
    do {
        esp_err_t r;
        // Create SCCB (I2C) interface based on Kconfig pins if present; fallback to defaults
        // Using example Kconfig symbols if available; will guard by #ifdef to avoid build errors.
    int sda=-1, scl=-1, i2c_port=-1, reset_pin=-1, pwdn_pin=-1, xclk_pin=-1; int32_t xclk_freq=24000000; // typical 24MHz
#ifdef CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SDA_PIN
        sda = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SDA_PIN;
#endif
#ifdef CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SCL_PIN
        scl = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SCL_PIN;
#endif
#ifdef CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_PORT
        i2c_port = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_PORT;
#endif
#ifdef CONFIG_EXAMPLE_MIPI_CSI_CAM_SENSOR_RESET_PIN
        reset_pin = CONFIG_EXAMPLE_MIPI_CSI_CAM_SENSOR_RESET_PIN;
#endif
#ifdef CONFIG_EXAMPLE_MIPI_CSI_CAM_SENSOR_PWDN_PIN
        pwdn_pin = CONFIG_EXAMPLE_MIPI_CSI_CAM_SENSOR_PWDN_PIN;
#endif
#ifdef CONFIG_EXAMPLE_MIPI_CSI_SCCB_XCLK_PIN
        xclk_pin = CONFIG_EXAMPLE_MIPI_CSI_SCCB_XCLK_PIN;
#endif
    // Silence unused variable warnings (Kconfig pins may be informative later for diagnostics)
    (void)sda; (void)scl; (void)i2c_port;
        ESP_LOGI(TAG, "cam pins: sda=%d scl=%d port=%d reset=%d pwdn=%d xclk=%d freq=%ld", sda,scl,i2c_port,reset_pin,pwdn_pin,xclk_pin,(long)xclk_freq);
        // Perform hardware sequence if pins are valid
        cam_hw_sequence(reset_pin, pwdn_pin);
        // Obtain existing board I2C bus (already initialized by BSP earlier in boot path)
        i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
        if (!i2c_bus){ ESP_LOGW(TAG, "bsp_i2c_get_handle returned NULL; stub snapshot retained"); break; }
        if (xclk_pin < 0){
            ESP_LOGW(TAG, "xclk pin undefined; sensor may not receive clock");
        }
        // Optional manual probe before formal detect (gives earlier feedback if ID registers are dead)
    bool manual_ok=false;
    manual_ok = cam_probe_ov5647(i2c_bus, 400000);
    if (!manual_ok){ vTaskDelay(pdMS_TO_TICKS(5)); manual_ok = cam_probe_ov5647(i2c_bus, 100000); }
    if (!manual_ok){ vTaskDelay(pdMS_TO_TICKS(5)); manual_ok = cam_probe_ov5647(i2c_bus, 50000); }
        // Try detection at graduated speeds (400k, 100k, 50k)
        s_sensor = cam_try_detect(i2c_bus, reset_pin, pwdn_pin, xclk_pin, xclk_freq, 400000);
        if (!s_sensor){ vTaskDelay(pdMS_TO_TICKS(20)); s_sensor = cam_try_detect(i2c_bus, reset_pin, pwdn_pin, xclk_pin, xclk_freq, 100000); }
        if (!s_sensor){ vTaskDelay(pdMS_TO_TICKS(20)); s_sensor = cam_try_detect(i2c_bus, reset_pin, pwdn_pin, xclk_pin, xclk_freq, 50000); }
        if (!s_sensor){
            ESP_LOGW(TAG, "no camera sensor (OV5647) detected after multi-speed attempts (up to 50kHz) manual_probe=%s", manual_ok?"id_seen":"no_id");
            break;
        }
        // Query supported formats and pick preferred JPEG
        esp_cam_sensor_format_array_t fmts={0};
        if (esp_cam_sensor_query_format(s_sensor, &fmts)==ESP_OK && fmts.count>0){
            const esp_cam_sensor_format_t *chosen=NULL;
            for (size_t p=0; p<ARRAY_SIZE(s_pref_list) && !chosen; ++p){
                for (uint32_t i=0;i<fmts.count;i++){
                    const esp_cam_sensor_format_t *f = &fmts.format_array[i];
                    if (f->format==ESP_CAM_SENSOR_PIXFORMAT_JPEG && f->width==s_pref_list[p].w && f->height==s_pref_list[p].h){ chosen=f; break; }
                }
            }
            if (!chosen){
                for (uint32_t i=0;i<fmts.count;i++){ // any JPEG as fallback
                    if (fmts.format_array[i].format==ESP_CAM_SENSOR_PIXFORMAT_JPEG){ chosen=&fmts.format_array[i]; break; }
                }
            }
            if (chosen){
                if (esp_cam_sensor_set_format(s_sensor, chosen)==ESP_OK){
                    ESP_LOGI(TAG, "format set %s %ux%u fps=%u", chosen->name, chosen->width, chosen->height, chosen->fps);
                } else {
                    ESP_LOGW(TAG, "failed set format; continuing with sensor but snapshot disabled");
                }
            } else {
                ESP_LOGW(TAG, "no JPEG format available; using stub");
            }
        } else {
            ESP_LOGW(TAG, "query format failed; using stub");
        }
        // Apply basic parameters (brightness/contrast/saturation) if sensor supports
        esp_cam_sensor_set_para_value(s_sensor, ESP_CAM_SENSOR_BRIGHTNESS, &s_cfg.brightness, sizeof(int));
        esp_cam_sensor_set_para_value(s_sensor, ESP_CAM_SENSOR_CONTRAST, &s_cfg.contrast, sizeof(int));
        esp_cam_sensor_set_para_value(s_sensor, ESP_CAM_SENSOR_SATURATION, &s_cfg.saturation, sizeof(int));
        // Start stream (arg=1) if required by driver for capture; some drivers might auto-stream on format set.
        int one=1; esp_cam_sensor_ioctl(s_sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &one);
        s_sensor_ready = true;
    } while(0);
    if (s_sensor_ready){ ESP_LOGI(TAG, "camera sensor ready"); }
    else { ESP_LOGI(TAG, "camera sensor not ready; stub snapshots active"); }
    return ESP_OK;
}

void opdi_cam_get_config(opdi_cam_config_t *out){ if(out) *out = s_cfg; }

esp_err_t opdi_cam_set_config(const opdi_cam_config_t *cfg){
    if(!cfg) return ESP_ERR_INVALID_ARG; 
    opdi_cam_config_t c = *cfg; cam_cfg_clamp(&c); s_cfg = c;
    nvs_handle_t h; esp_err_t err = nvs_open(CAM_NVS_NS, NVS_READWRITE, &h);
    if (err!=ESP_OK) return err;
    err = nvs_set_blob(h, CAM_NVS_KEY, &s_cfg, sizeof(s_cfg));
    if (err==ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "config saved (b=%d c=%d s=%d ae=%d)", s_cfg.brightness, s_cfg.contrast, s_cfg.saturation, s_cfg.auto_exposure);
    return err;
}

int opdi_cam_snapshot(unsigned char *buf, size_t buf_cap){
    // Fallback early if sensor not ready
    if (!s_sensor_ready){
        int need = (int)sizeof(s_jpeg_stub);
        if (!buf) return need;
        if ((size_t)need > buf_cap) return -need;
        memcpy(buf, s_jpeg_stub, need);
        return need;
    }
    // Initialize esp_video pipeline lazily
    #ifdef OPDI_CAM_HAVE_VIDEO
    if (!s_video_inited){
        video_device_config_t vcfg = {
            .type = VIDEO_DEVICE_TYPE_CAMERA,
            .flags = 0,
        };
        if (video_new_device(&vcfg, &s_video_dev)==ESP_OK){
            s_video_inited = true;
            ESP_LOGI(TAG, "esp_video device created");
        } else {
            ESP_LOGW(TAG, "esp_video device create failed; using stub");
        }
    }
    if (!s_video_inited){
        int need = (int)sizeof(s_jpeg_stub);
        if (!buf) return need;
        if ((size_t)need > buf_cap) return -need;
        memcpy(buf, s_jpeg_stub, need);
        return need;
    }
    // Request a frame (blocking capture). We assume sensor already streaming; capture API returns raw or encoded.
    video_frame_t frame = {0};
    esp_err_t r = video_device_take_frame(s_video_dev, &frame, 100 / portTICK_PERIOD_MS);
    if (r!=ESP_OK || frame.buf==NULL || frame.len==0){
        if (r!=ESP_OK) ESP_LOGW(TAG, "video_device_take_frame err=%s", esp_err_to_name(r));
        int need = (int)sizeof(s_jpeg_stub);
        if (!buf) return need;
        if ((size_t)need > buf_cap) return -need;
        memcpy(buf, s_jpeg_stub, need);
        return need;
    }
    // If caller only queries size
    if (!buf){
        int sz = (int)frame.len;
        video_device_return_frame(s_video_dev, &frame);
        return sz;
    }
    if (frame.len > buf_cap){
        int need = (int)frame.len;
        video_device_return_frame(s_video_dev, &frame);
        return -need; // indicate required size
    }
    memcpy(buf, frame.buf, frame.len);
    int ret = (int)frame.len;
    video_device_return_frame(s_video_dev, &frame);
    return ret;
    #else
    // esp_video not integrated -> fallback stub
    int need = (int)sizeof(s_jpeg_stub);
    if (!buf) return need;
    if ((size_t)need > buf_cap) return -need;
    memcpy(buf, s_jpeg_stub, need);
    return need;
    #endif
}
