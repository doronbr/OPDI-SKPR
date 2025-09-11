// Telemetry scaffolding
// Telemetry & frame stats implementation
#include "opdi_cam.h"
#include "esp_timer.h"
#include <string.h>
#include "opdi_api_ws.h"
#include "esp_log.h"

static const char *TAG_TEL = "cam_tel";

static opdi_cam_telemetry_t s_tel; // zeroed
static uint32_t s_frame_count_capture = 0;
static uint32_t s_frame_count_stream = 0; // placeholder until streaming integrated
static uint32_t s_last_sec_time = 0; // seconds

// Exposed accessor
void opdi_cam_get_telemetry(opdi_cam_telemetry_t *out){ if(out) *out = s_tel; }

// Seed initial telemetry values (called from manager init)
void opdi_cam_telemetry_seed(opdi_cam_profile_t prof, uint8_t jpeg_q, uint8_t fps_target, opdi_ir_mode_t ir_mode){
	s_tel.active_profile = prof;
	s_tel.jpeg_q_current = jpeg_q;
	s_tel.fps_target = fps_target;
	s_tel.ir_mode_cfg = ir_mode;
}

// Called on each captured frame (from snapshot path or future streaming task)
void opdi_cam_on_frame(uint16_t luma_avg){
	s_frame_count_capture++;
	s_tel.luma_avg = luma_avg;
	// IR policy evaluation (simple threshold only; full hysteresis in IR module later)
	extern void opdi_cam_ir_eval(uint16_t y_avg); // weak symbol (we'll add in ir file)
	opdi_cam_ir_eval(luma_avg);
}

// Periodic 1s tick -> update fps & drop metrics
void opdi_cam_periodic_1s(void){
	uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
	if (!s_last_sec_time){ s_last_sec_time = now_s; return; }
	uint32_t delta = now_s - s_last_sec_time;
	if (delta == 0) return;
	s_last_sec_time = now_s;
	s_tel.fps_capture = (uint8_t)(s_frame_count_capture / delta);
	s_tel.fps_stream = (uint8_t)(s_frame_count_stream / delta);
	s_frame_count_capture = 0;
	s_frame_count_stream = 0;
	// allow streaming module to refine stream fps & drop % (will call back)
	extern void opdi_cam_stream_periodic_1s(void);
	opdi_cam_stream_periodic_1s();
	// Governor evaluation after metrics
	extern void opdi_cam_governor_periodic(void);
	opdi_cam_governor_periodic();
	// Broadcast telemetry over WS
	char buf[256];
	int n = snprintf(buf, sizeof(buf),
		"{\"type\":\"cam.telemetry\",\"profile\":%u,\"fps\":%u,\"fps_stream\":%u,\"jpeg_q\":%u,\"luma\":%u,\"ir_mode\":%u,\"ir_active\":%s}",
		(unsigned)s_tel.active_profile, s_tel.fps_capture, s_tel.fps_stream, s_tel.jpeg_q_current,
		s_tel.luma_avg, (unsigned)s_tel.ir_mode_cfg, s_tel.ir_active?"true":"false");
	opdi_api_ws_broadcast(buf, (size_t)n);
	ESP_LOGD(TAG_TEL, "ws cam.telemetry sent");
}

// Called by streaming module
void opdi_cam_adjust_stream_metrics(uint8_t fps_stream, uint8_t drop_pct){
	s_tel.fps_stream = fps_stream;
	s_tel.drop_pct = drop_pct;
}
