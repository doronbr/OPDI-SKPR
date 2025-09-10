#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Simple camera config structure (Phase1 placeholder)
typedef struct {
    int brightness; // -10..10 (placeholder range)
    int contrast;   // -10..10
    int saturation; // -10..10
    bool auto_exposure;
} opdi_cam_config_t;

// Initialize camera subsystem (load config, init sensor later Phase2)
esp_err_t opdi_cam_init(void);

// Get current config
void opdi_cam_get_config(opdi_cam_config_t *out);

// Set (and persist) new config
esp_err_t opdi_cam_set_config(const opdi_cam_config_t *cfg);

// Capture snapshot into provided buffer. If buf==NULL returns required size.
// For Phase1 returns a tiny static JPEG test pattern.
int opdi_cam_snapshot(unsigned char *buf, size_t buf_cap);

#ifdef __cplusplus
}
#endif
