/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"

static const char *TAG = "bsp_extra_board";
static uint8_t s_es8311_detected_addr = 0; // store detected 7-bit address

static esp_codec_dev_handle_t play_dev_handle;
static esp_codec_dev_handle_t record_dev_handle;

static bool _is_audio_init = false;
static bool _is_player_init = false;
static int _vloume_intensity = CODEC_DEFAULT_VOLUME;

/* Fallback defaults in case new Kconfig options not yet merged into sdkconfig */
#ifndef CONFIG_EXAMPLE_AUDIO_I2C_RETRY_COUNT
#define CONFIG_EXAMPLE_AUDIO_I2C_RETRY_COUNT 3
#endif
#ifndef CONFIG_EXAMPLE_AUDIO_I2C_SCAN_BEFORE_INIT
#define CONFIG_EXAMPLE_AUDIO_I2C_SCAN_BEFORE_INIT 0
#endif
#ifndef CONFIG_EXAMPLE_AUDIO_CODEC_GRACEFUL_FAIL
#define CONFIG_EXAMPLE_AUDIO_CODEC_GRACEFUL_FAIL 1
#endif

static audio_player_cb_t audio_idle_callback = NULL;
static void *audio_idle_cb_user_data = NULL;
static char audio_file_path[128];

/**************************************************************************************************
 *
 * Extra Board Function
 *
 **************************************************************************************************/

static esp_err_t audio_mute_function(AUDIO_PLAYER_MUTE_SETTING setting)
{
    // Volume saved when muting and restored when unmuting. Restoring volume is necessary
    // as es8311_set_voice_mute(true) results in voice volume (REG32) being set to zero.

    bsp_extra_codec_mute_set(setting == AUDIO_PLAYER_MUTE ? true : false);

    // restore the voice volume upon unmuting
    if (setting == AUDIO_PLAYER_UNMUTE) {
        ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(play_dev_handle, _vloume_intensity), TAG, "Set Codec volume failed");
    }

    return ESP_OK;
}

static void audio_callback(audio_player_cb_ctx_t *ctx)
{
    if (audio_idle_callback) {
        ctx->user_ctx = audio_idle_cb_user_data;
        audio_idle_callback(ctx);
    }
}

esp_err_t bsp_extra_i2s_read(void *audio_buffer, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
    esp_err_t ret = ESP_OK;
    ret = esp_codec_dev_read(record_dev_handle, audio_buffer, len);
    *bytes_read = len;
    return ret;
}

esp_err_t bsp_extra_i2s_write(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    esp_err_t ret = ESP_OK;
    ret = esp_codec_dev_write(play_dev_handle, audio_buffer, len);
    *bytes_written = len;
    return ret;
}

esp_err_t bsp_extra_codec_set_fs(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    esp_err_t ret = ESP_OK;

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = rate,
        .channel = ch,
        .bits_per_sample = bits_cfg,
    };

    if (play_dev_handle) {
        ret = esp_codec_dev_close(play_dev_handle);
    }
    if (record_dev_handle) {
        ret |= esp_codec_dev_close(record_dev_handle);
        ret |= esp_codec_dev_set_in_gain(record_dev_handle, CODEC_DEFAULT_ADC_VOLUME);
    }

    if (play_dev_handle) {
        ret |= esp_codec_dev_open(play_dev_handle, &fs);
    }
    if (record_dev_handle) {
        ret |= esp_codec_dev_open(record_dev_handle, &fs);
    }
    return ret;
}

esp_err_t bsp_extra_codec_volume_set(int volume, int *volume_set)
{
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(play_dev_handle, volume), TAG, "Set Codec volume failed");
    _vloume_intensity = volume;

    ESP_LOGI(TAG, "Setting volume: %d", volume);
    if (volume_set) {
        *volume_set = _vloume_intensity;
    }
    return ESP_OK;
}

int bsp_extra_codec_volume_get(void)
{
    return _vloume_intensity;
}

esp_err_t bsp_extra_codec_mute_set(bool enable)
{
    esp_err_t ret = ESP_OK;
    ret = esp_codec_dev_set_out_mute(play_dev_handle, enable);
    return ret;
}

esp_err_t bsp_extra_codec_dev_stop(void)
{
    esp_err_t ret = ESP_OK;

    if (play_dev_handle) {
        ret = esp_codec_dev_close(play_dev_handle);
    }

    if (record_dev_handle) {
        ret = esp_codec_dev_close(record_dev_handle);
    }
    return ret;
}

esp_err_t bsp_extra_codec_dev_resume(void)
{
    return bsp_extra_codec_set_fs(CODEC_DEFAULT_SAMPLE_RATE, CODEC_DEFAULT_BIT_WIDTH, CODEC_DEFAULT_CHANNEL);
}

static bool _probe_es8311_address(void)
{
#if CONFIG_EXAMPLE_AUDIO_I2C_SCAN_BEFORE_INIT
    /* Delay to allow power rails / reset to settle */
    vTaskDelay(pdMS_TO_TICKS(20));
    extern i2c_master_bus_handle_t bsp_i2c_get_handle(void); // from board code
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        // Try to initialize I2C now (lazy init) so scan can proceed
        if (bsp_i2c_init() == ESP_OK) {
            bus = bsp_i2c_get_handle();
        }
    }
    if (!bus) {
        ESP_LOGW(TAG, "I2C bus handle not available for ES8311 probe");
        return false;
    }
    /* ES8311 7-bit base address typically 0x18 (ADDR pin low) or 0x19 (ADDR pin high).
       Some legacy code defines 8-bit write addr 0x30. We'll test a small set. */
    const uint8_t candidates[] = { 0x18, 0x19, (uint8_t)(ES8311_CODEC_DEFAULT_ADDR & 0x7F) };
#if CONFIG_EXAMPLE_AUDIO_I2C_DEBUG_SCAN
    ESP_LOGI(TAG, "ES8311 debug scan start");
#endif
    for (size_t i = 0; i < sizeof(candidates); ++i) {
        uint8_t addr = candidates[i];
        if (addr == 0) continue;
        i2c_device_config_t dev_cfg = {
            .device_address = addr,
            .scl_speed_hz = 100000,
        };
        i2c_master_dev_handle_t dev;
        if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
            continue;
        }
        uint8_t reg = 0x00; /* First register read often valid */
        uint8_t val = 0;
        esp_err_t ok = i2c_master_transmit_receive(dev, &reg, 1, &val, 1, 20);
#if CONFIG_EXAMPLE_AUDIO_I2C_DEBUG_SCAN
        ESP_LOGI(TAG, "Probe addr 0x%02X -> %s (err=0x%x val=0x%02X)", addr, (ok==ESP_OK)?"ACK":"NACK", ok, val);
#endif
        i2c_master_bus_rm_device(dev);
        if (ok == ESP_OK) {
            ESP_LOGI(TAG, "ES8311 detected at 7-bit addr 0x%02X", addr);
            s_es8311_detected_addr = addr;
            return true;
        }
    }
#if CONFIG_EXAMPLE_AUDIO_I2C_DEBUG_SCAN
    // Optional wider scan in 0x10-0x1F range for diagnostics
    for (uint8_t addr = 0x10; addr <= 0x1F; ++addr) {
        bool already = false;
        for (size_t j = 0; j < sizeof(candidates); ++j) if (candidates[j] == addr) already = true;
        if (already) continue;
        i2c_device_config_t dev_cfg = { .device_address = addr, .scl_speed_hz = 100000 };
        i2c_master_dev_handle_t dev;
        if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) continue;
        uint8_t reg = 0x00, val = 0;
        esp_err_t ok = i2c_master_transmit_receive(dev, &reg, 1, &val, 1, 15);
        i2c_master_bus_rm_device(dev);
        ESP_LOGI(TAG, "Scan addr 0x%02X -> %s (err=0x%x)", addr, (ok==ESP_OK)?"ACK":"NACK", ok);
    }
#endif
    return false;
#else
    return true;
#endif
}

esp_err_t bsp_extra_codec_init()
{
    if (_is_audio_init) {
        return ESP_OK;
    }
#if CONFIG_EXAMPLE_ENABLE_AUDIO_CODEC
    int retries = CONFIG_EXAMPLE_AUDIO_I2C_RETRY_COUNT;
    esp_err_t last_err = ESP_FAIL;
    while (retries-- > 0) {
        if (!_probe_es8311_address()) {
            ESP_LOGW(TAG, "ES8311 address probe failed (attempt %d).", CONFIG_EXAMPLE_AUDIO_I2C_RETRY_COUNT - retries);
            vTaskDelay(pdMS_TO_TICKS(60));
            continue;
        }
        if (s_es8311_detected_addr) {
            // Override global macro use by temporarily modifying a weak symbol if available (future improvement)
            ESP_LOGI(TAG, "Initializing codec with detected addr 0x%02X", s_es8311_detected_addr);
        }
        play_dev_handle = bsp_audio_codec_speaker_init();
        if (!play_dev_handle) {
            last_err = ESP_FAIL;
            ESP_LOGE(TAG, "Speaker codec init failed (attempt %d)", CONFIG_EXAMPLE_AUDIO_I2C_RETRY_COUNT - retries);
            vTaskDelay(pdMS_TO_TICKS(60));
            continue;
        }
        record_dev_handle = bsp_audio_codec_microphone_init();
        if (!record_dev_handle) {
            last_err = ESP_FAIL;
            ESP_LOGE(TAG, "Mic codec init failed (attempt %d)", CONFIG_EXAMPLE_AUDIO_I2C_RETRY_COUNT - retries);
            vTaskDelay(pdMS_TO_TICKS(60));
            continue;
        }
        /* Success */
        bsp_extra_codec_set_fs(CODEC_DEFAULT_SAMPLE_RATE, CODEC_DEFAULT_BIT_WIDTH, CODEC_DEFAULT_CHANNEL);
        _is_audio_init = true;
        ESP_LOGI(TAG, "Audio codec initialized successfully");
        return ESP_OK;
    }
    if (!_is_audio_init) {
#if CONFIG_EXAMPLE_AUDIO_CODEC_GRACEFUL_FAIL
        ESP_LOGE(TAG, "Audio codec initialization failed after retries. Continuing without audio.");
        return ESP_FAIL;
#else
        assert(!"Audio codec initialization failed");
#endif
    }
    return last_err;
#else
    ESP_LOGW(TAG, "Audio codec disabled at compile time");
    return ESP_OK;
#endif
}

esp_err_t bsp_extra_player_init(void)
{
    if (_is_player_init) {
        return ESP_OK;
    }

    audio_player_config_t config = { .mute_fn = audio_mute_function,
                                     .write_fn = bsp_extra_i2s_write,
                                     .clk_set_fn = bsp_extra_codec_set_fs,
                                     .priority = 5
                                   };
    ESP_RETURN_ON_ERROR(audio_player_new(config), TAG, "audio_player_init failed");
    audio_player_callback_register(audio_callback, NULL);

    _is_player_init = true;

    return ESP_OK;
}

esp_err_t bsp_extra_player_del(void)
{
    _is_player_init = false;

    ESP_RETURN_ON_ERROR(audio_player_delete(), TAG, "audio_player_delete failed");

    return ESP_OK;
}

esp_err_t bsp_extra_file_instance_init(const char *path, file_iterator_instance_t **ret_instance)
{
    ESP_RETURN_ON_FALSE(path, ESP_FAIL, TAG, "path is NULL");
    ESP_RETURN_ON_FALSE(ret_instance, ESP_FAIL, TAG, "ret_instance is NULL");

    file_iterator_instance_t *file_iterator = file_iterator_new(path);
    ESP_RETURN_ON_FALSE(file_iterator, ESP_FAIL, TAG, "file_iterator_new failed, %s", path);

    *ret_instance = file_iterator;

    return ESP_OK;
}

esp_err_t bsp_extra_player_play_index(file_iterator_instance_t *instance, int index)
{
    ESP_RETURN_ON_FALSE(instance, ESP_FAIL, TAG, "instance is NULL");

    ESP_LOGI(TAG, "play_index(%d)", index);
    char filename[128];
    int retval = file_iterator_get_full_path_from_index(instance, index, filename, sizeof(filename));
    ESP_RETURN_ON_FALSE(retval != 0, ESP_FAIL, TAG, "file_iterator_get_full_path_from_index failed");

    ESP_LOGI(TAG, "opening file '%s'", filename);
    FILE *fp = fopen(filename, "rb");
    ESP_RETURN_ON_FALSE(fp, ESP_FAIL, TAG, "unable to open file");

    ESP_LOGI(TAG, "Playing '%s'", filename);
    ESP_RETURN_ON_ERROR(audio_player_play(fp), TAG, "audio_player_play failed");

    memcpy(audio_file_path, filename, sizeof(audio_file_path));

    return ESP_OK;
}

esp_err_t bsp_extra_player_play_file(const char *file_path)
{
    ESP_LOGI(TAG, "opening file '%s'", file_path);
    FILE *fp = fopen(file_path, "rb");
    ESP_RETURN_ON_FALSE(fp, ESP_FAIL, TAG, "unable to open file");

    ESP_LOGI(TAG, "Playing '%s'", file_path);
    ESP_RETURN_ON_ERROR(audio_player_play(fp), TAG, "audio_player_play failed");

    memcpy(audio_file_path, file_path, sizeof(audio_file_path));

    return ESP_OK;
}

void bsp_extra_player_register_callback(audio_player_cb_t cb, void *user_data)
{
    audio_idle_callback = cb;
    audio_idle_cb_user_data = user_data;
}

bool bsp_extra_player_is_playing_by_path(const char *file_path)
{
    return (strcmp(audio_file_path, file_path) == 0);
}

bool bsp_extra_player_is_playing_by_index(file_iterator_instance_t *instance, int index)
{
    return (index == file_iterator_get_index(instance));
}