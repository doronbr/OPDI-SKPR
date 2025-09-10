#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"  // Kconfig generated symbols
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_memory_utils.h"
#include "esp_heap_caps.h"
#include <sys/stat.h>
#include <math.h>
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"

#include "esp_brookesia.hpp"
#include "app_examples/phone/squareline/src/phone_app_squareline.hpp"
#include "apps.h"

static const char *TAG = "main";

static void startup_audio_task(void *arg)
{
    (void)arg;
#if CONFIG_EXAMPLE_ENABLE_AUDIO_CODEC
#ifndef CONFIG_EXAMPLE_STARTUP_VOLUME
#define CONFIG_EXAMPLE_STARTUP_VOLUME 90
#endif
#ifndef CONFIG_EXAMPLE_STARTUP_FILE_PATH
#define CONFIG_EXAMPLE_STARTUP_FILE_PATH "/spiffs/2048/good.mp3"
#endif
#ifndef CONFIG_EXAMPLE_STARTUP_BEEP_ENABLE
#define CONFIG_EXAMPLE_STARTUP_BEEP_ENABLE 1
#endif
#ifndef CONFIG_EXAMPLE_STARTUP_BEEP_FREQ
#define CONFIG_EXAMPLE_STARTUP_BEEP_FREQ 440
#endif
#ifndef CONFIG_EXAMPLE_STARTUP_BEEP_DURATION_MS
#define CONFIG_EXAMPLE_STARTUP_BEEP_DURATION_MS 120
#endif
#ifndef CONFIG_EXAMPLE_AUDIO_ASYNC_STARTUP
#define CONFIG_EXAMPLE_AUDIO_ASYNC_STARTUP 0
#endif
    // Set volume first
    int volume_set = 0;
    if (bsp_extra_codec_volume_set(CONFIG_EXAMPLE_STARTUP_VOLUME, &volume_set) == ESP_OK) {
        ESP_LOGI(TAG, "Volume set to %d%% (reported=%d)", CONFIG_EXAMPLE_STARTUP_VOLUME, volume_set);
    } else {
        ESP_LOGW(TAG, "Failed to set startup volume to %d%%", CONFIG_EXAMPLE_STARTUP_VOLUME);
    }

    const char *startup_sound = CONFIG_EXAMPLE_STARTUP_FILE_PATH; // configurable path
    bool played = false;
    struct stat st;
    if (startup_sound[0] != '\0' && stat(startup_sound, &st) == 0) {
        esp_err_t play_res = bsp_extra_player_play_file(startup_sound);
        if (play_res == ESP_OK) {
            ESP_LOGI(TAG, "Startup sound playing: %s (%ld bytes)", startup_sound, (long)st.st_size);
            played = true;
        } else {
            ESP_LOGW(TAG, "Failed to play startup sound (%s): %s", startup_sound, esp_err_to_name(play_res));
        }
    } else {
        if (startup_sound[0] != '\0') {
            ESP_LOGW(TAG, "Startup sound file not found: %s", startup_sound);
        } else {
            ESP_LOGI(TAG, "Startup sound path empty; skipping file playback");
        }
    }

#if CONFIG_EXAMPLE_STARTUP_BEEP_ENABLE
    if (!played) {
        const int sample_rate = CODEC_DEFAULT_SAMPLE_RATE; // 16000
        const int duration_ms = CONFIG_EXAMPLE_STARTUP_BEEP_DURATION_MS;
        const int total_samples = sample_rate * duration_ms / 1000;
        const float freq = (float)CONFIG_EXAMPLE_STARTUP_BEEP_FREQ;
        const float amplitude = 0.25f; // 25% of full scale
        size_t frame_bytes = 2 * CODEC_DEFAULT_CHANNEL; // 16-bit * channels
        int16_t *pcm = (int16_t *)heap_caps_malloc(total_samples * frame_bytes, MALLOC_CAP_8BIT);
        if (pcm) {
            for (int i = 0; i < total_samples; ++i) {
                float s = sinf(2.0f * 3.14159265f * freq * i / sample_rate);
                int16_t v = (int16_t)(s * 32767.0f * amplitude);
                pcm[2 * i + 0] = v;
                pcm[2 * i + 1] = v; // stereo duplicate
            }
            size_t written = 0;
            if (bsp_extra_i2s_write(pcm, total_samples * frame_bytes, &written, 200) == ESP_OK) {
                ESP_LOGI(TAG, "Fallback tone played (%d samples, %u bytes, %d Hz)", total_samples, (unsigned)written, CONFIG_EXAMPLE_STARTUP_BEEP_FREQ);
            } else {
                ESP_LOGW(TAG, "Failed to play fallback tone");
            }
            free(pcm);
        } else {
            ESP_LOGW(TAG, "Failed to allocate buffer for fallback tone");
        }
    }
#else
    if (!played) {
        ESP_LOGI(TAG, "Startup file not played and beep disabled; staying silent");
    }
#endif // CONFIG_EXAMPLE_STARTUP_BEEP_ENABLE

#endif // CONFIG_EXAMPLE_ENABLE_AUDIO_CODEC
    vTaskDelete(nullptr);
}

// --- Network & HTTP integration (merged from previous app_main.c) ---
#include "esp_http_server.h"
#include "opdi_net.h"
#include "opdi_api_ws.h"
#include "opdi_cam.h"

// Forward declarations for previously separate C files (ensure C linkage)
#ifdef __cplusplus
extern "C" {
#endif
void routes_net_register(httpd_handle_t server);
void opdi_api_static_register(httpd_handle_t server);
#ifdef __cplusplus
}
#endif

// System info endpoint handler (moved to file scope from inside opdi_start_httpd)
static esp_err_t sysinfo_get(httpd_req_t *r) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"device\":\"OPDI_SKPR\",\"idf\":\"%s\",\"uptime_s\":%ld}",
        esp_get_idf_version(), (long)(esp_timer_get_time()/1000000LL));
    httpd_resp_set_type(r, "application/json");
    httpd_resp_send(r, buf, n);
    return ESP_OK;
}

static httpd_handle_t opdi_start_httpd(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    // We register a fairly large set of endpoints (system info + ~10 net REST routes + metrics/logs +
    // websocket endpoint + several static file handlers). The default (typically 8) is insufficient
    // and produced 'httpd_register_uri_handler: no slots left' warnings. Bump this to provide headroom.
    cfg.max_uri_handlers = 24;
    httpd_handle_t h = NULL;
    if (httpd_start(&h, &cfg) != ESP_OK) return NULL;
    httpd_uri_t u_sys = { .uri = "/api/v1/system/info", .method = HTTP_GET, .handler = sysinfo_get };
    httpd_register_uri_handler(h, &u_sys);
    // Register networking routes, websocket endpoint and static UI assets
    routes_net_register(h);
    opdi_api_ws_register(h);
    opdi_api_static_register(h);
    ESP_LOGI(TAG, "HTTP server started (port %d)", cfg.server_port);
    return h;
}

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(bsp_spiffs_mount());
    ESP_LOGI(TAG, "SPIFFS mount successfully");

#if CONFIG_EXAMPLE_ENABLE_SD_CARD
    ESP_ERROR_CHECK(bsp_sdcard_mount());
    ESP_LOGI(TAG, "SD card mount successfully");
#endif

    #if CONFIG_EXAMPLE_ENABLE_AUDIO_CODEC
    esp_err_t audio_err = bsp_extra_codec_init();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "Audio codec not initialized (err=%s). Continuing without audio.", esp_err_to_name(audio_err));
    } else {
        if (bsp_extra_player_init() == ESP_OK) {
#if CONFIG_EXAMPLE_AUDIO_ASYNC_STARTUP
            BaseType_t task_ok = xTaskCreate(startup_audio_task, "startup_audio", 4096, nullptr, tskIDLE_PRIORITY + 2, nullptr);
            if (task_ok != pdPASS) {
                ESP_LOGW(TAG, "Failed to create startup audio task; running inline");
                startup_audio_task(nullptr);
            }
#else
            startup_audio_task(nullptr);
#endif
        } else {
            ESP_LOGW(TAG, "Audio player init failed; skipping startup audio sequence");
        }
    }
    #else
    ESP_LOGW(TAG, "Audio codec disabled (CONFIG_EXAMPLE_ENABLE_AUDIO_CODEC=n). Skipping ES8311 init.");
    #endif // CONFIG_EXAMPLE_ENABLE_AUDIO_CODEC

    #if CONFIG_EXAMPLE_ENABLE_DISPLAY && !CONFIG_EXAMPLE_HEADLESS_MODE
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = false,
        }
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    bsp_display_lock(0);
    #else
    ESP_LOGW(TAG, "UI initialization skipped (display disabled or headless mode)");
    extern void brookesia_disable_ui(void); // weak symbol placeholder
    brookesia_disable_ui();
    #endif // CONFIG_EXAMPLE_ENABLE_DISPLAY && !CONFIG_EXAMPLE_HEADLESS_MODE

    // Initialize camera config (Phase1 stub)
    opdi_cam_init();

    // Network manager + HTTP server bootstrap (after storage ready)
    opdi_net_init();
    opdi_start_httpd();
    ESP_LOGI(TAG, "OPDI_SKPR network stack initialized");

    // If headless or display disabled, avoid creating heavy UI objects
#if CONFIG_EXAMPLE_HEADLESS_MODE || !CONFIG_EXAMPLE_ENABLE_DISPLAY
    ESP_LOGI(TAG, "Skipping Brookesia Phone object creation (headless or display disabled)");
#else
    ESP_Brookesia_Phone *phone = new ESP_Brookesia_Phone();
    assert(phone != nullptr && "Failed to create phone");

    ESP_Brookesia_PhoneStylesheet_t *phone_stylesheet = new ESP_Brookesia_PhoneStylesheet_t ESP_BROOKESIA_PHONE_1024_600_DARK_STYLESHEET();
    ESP_BROOKESIA_CHECK_NULL_EXIT(phone_stylesheet, "Create phone stylesheet failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT(phone->addStylesheet(*phone_stylesheet), "Add phone stylesheet failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT(phone->activateStylesheet(*phone_stylesheet), "Activate phone stylesheet failed");

    assert(phone->begin() && "Failed to begin phone");

    PhoneAppSquareline *smart_gadget = new PhoneAppSquareline();
    assert(smart_gadget != nullptr && "Failed to create phone app squareline");
    assert((phone->installApp(smart_gadget) >= 0) && "Failed to install phone app squareline");

    Calculator *calculator = new Calculator();
    assert(calculator != nullptr && "Failed to create calculator");
    assert((phone->installApp(calculator) >= 0) && "Failed to begin calculator");

    MusicPlayer *music_player = new MusicPlayer();
    assert(music_player != nullptr && "Failed to create music_player");
    assert((phone->installApp(music_player) >= 0) && "Failed to begin music_player");

    AppSettings *app_settings = new AppSettings();
    assert(app_settings != nullptr && "Failed to create app_settings");
    assert((phone->installApp(app_settings) >= 0) && "Failed to begin app_settings");

    Game2048 *game_2048 = new Game2048();
    assert(game_2048 != nullptr && "Failed to create game_2048");
    assert((phone->installApp(game_2048) >= 0) && "Failed to begin game_2048");

    Camera *camera = new Camera(1280, 720);
    assert(camera != nullptr && "Failed to create camera");
    assert((phone->installApp(camera) >= 0) && "Failed to begin camera");

#if CONFIG_EXAMPLE_ENABLE_SD_CARD
    ESP_LOGW(TAG, "Using Video Player example requires inserting the SD card in advance and saving an MJPEG format video on the SD card");
    AppVideoPlayer *app_video_player = new AppVideoPlayer();
    assert(app_video_player != nullptr && "Failed to create app_video_player");
    assert((phone->installApp(app_video_player) >= 0) && "Failed to begin app_video_player");
#endif

    #if CONFIG_EXAMPLE_ENABLE_DISPLAY && !CONFIG_EXAMPLE_HEADLESS_MODE
    bsp_display_unlock();
    #endif
#endif // CONFIG_EXAMPLE_HEADLESS_MODE || !CONFIG_EXAMPLE_ENABLE_DISPLAY
}

extern "C" void brookesia_disable_ui(void) __attribute__((weak));
extern "C" void brookesia_disable_ui(void) {}
