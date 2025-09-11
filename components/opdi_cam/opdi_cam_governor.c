// Governor implementation (adaptive profile & JPEG quality)
#include "opdi_cam.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "cam_gov";

static uint8_t s_cpu_load_pct = 0;
static uint64_t s_last_downshift_us = 0;
static uint64_t s_last_upshift_us = 0;
static const uint64_t UPSHIFT_GUARD_US = 10ULL * 1000000ULL; // 10s

extern esp_err_t opdi_cam_ext_config_get(opdi_cam_ext_config_t *out);
extern esp_err_t opdi_cam_ext_config_set(const opdi_cam_ext_config_t *in);
extern void opdi_cam_get_telemetry(opdi_cam_telemetry_t *out);
extern void opdi_cam_on_frame(uint16_t luma_avg);
extern void opdi_cam_adjust_stream_metrics(uint8_t fps_stream, uint8_t drop_pct);
extern void opdi_cam_stream_stats(uint32_t *accepted, uint32_t *served, uint32_t *dropped);

void opdi_cam_governor_notify_cpu_load(uint8_t pct){ s_cpu_load_pct = pct; }

static void apply_profile(opdi_cam_profile_t newp){
	opdi_cam_ext_config_t c; opdi_cam_ext_config_get(&c);
	if (c.profile == newp) return;
	c.profile = newp; opdi_cam_ext_config_set(&c);
	ESP_LOGI(TAG, "profile change -> %d", (int)newp);
}

static void adjust_jpeg_q(int delta){
	opdi_cam_ext_config_t c; opdi_cam_ext_config_get(&c);
	int q = (int)c.jpeg_q + delta;
	if (q < 50) q=50; else if (q>90) q=90;
	if (q == c.jpeg_q) return;
	c.jpeg_q = (uint8_t)q; opdi_cam_ext_config_set(&c);
	ESP_LOGI(TAG, "jpeg_q -> %d", q);
}

// Called each second from periodic tick (after telemetry update) to evaluate scaling
void opdi_cam_governor_periodic(void){
#ifdef CONFIG_OPDI_CAM_GOVERNOR
	opdi_cam_ext_config_t c; opdi_cam_ext_config_get(&c);
	uint64_t now = esp_timer_get_time();
	uint32_t acc=0, served=0, drop=0; opdi_cam_stream_stats(&acc,&served,&drop);
	uint32_t backlog = (acc > served) ? (acc - served) : 0;
	bool high_load = (s_cpu_load_pct > 85) || (backlog > 3);
	bool low_load = (s_cpu_load_pct < 55) && (backlog < 2);

	// Downshift conditions
	if (high_load){
		if (c.profile == OPDI_CAM_PROFILE_720P){ apply_profile(OPDI_CAM_PROFILE_480P); s_last_downshift_us = now; }
		else if (c.profile == OPDI_CAM_PROFILE_480P){ apply_profile(OPDI_CAM_PROFILE_240P); s_last_downshift_us = now; }
		else { adjust_jpeg_q(-5); }
		return; // only one action per cycle
	}
	// Upshift conditions (guard time)
	if (low_load && (now - s_last_downshift_us) > UPSHIFT_GUARD_US && (now - s_last_upshift_us) > UPSHIFT_GUARD_US){
		if (c.profile == OPDI_CAM_PROFILE_240P){ apply_profile(OPDI_CAM_PROFILE_480P); s_last_upshift_us = now; }
		else if (c.profile == OPDI_CAM_PROFILE_480P){ apply_profile(OPDI_CAM_PROFILE_720P); s_last_upshift_us = now; }
		else { adjust_jpeg_q(+5); }
	}
#endif
}
