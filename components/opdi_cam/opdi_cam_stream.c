// Streaming ring buffer implementation (logic layer only)
#include "opdi_cam.h"
#include "esp_timer.h"
#include <string.h>

#ifndef CONFIG_OPDI_CAM_STREAM_DEPTH
#define CONFIG_OPDI_CAM_STREAM_DEPTH 3
#endif

typedef struct {
	uint32_t ts_ms;
	size_t len;
	uint8_t jpeg_q;
	opdi_cam_profile_t profile;
	uint8_t *buf; // allocated block
} cam_stream_slot_t;

static cam_stream_slot_t s_slots[CONFIG_OPDI_CAM_STREAM_DEPTH];
static size_t s_slot_cap = 0; // capacity per slot (largest seen so far)
static int s_latest = -1; // index of newest frame
static uint32_t s_dropped = 0;
static uint32_t s_accepted = 0;
static uint32_t s_served = 0; // number of frames handed to clients via copy

size_t opdi_cam_stream_current_frame_size(void){
	if (s_latest < 0) {
		return 0;
	}
	return s_slots[s_latest].len;
}

esp_err_t opdi_cam_stream_push_jpeg(const uint8_t *data, size_t len, opdi_cam_profile_t profile, uint8_t jpeg_q){
	if (!data || !len) return ESP_ERR_INVALID_ARG;
	// Choose next slot (overwrite oldest if full). Strategy: advance circularly.
	int next = (s_latest + 1) % CONFIG_OPDI_CAM_STREAM_DEPTH;
	cam_stream_slot_t *slot = &s_slots[next];
	if (!slot->buf || s_slot_cap < len){
		// Reallocate slot buffer
		uint8_t *nb = (uint8_t*)realloc(slot->buf, len);
		if (!nb){ s_dropped++; return ESP_FAIL; }
		slot->buf = nb; s_slot_cap = len > s_slot_cap ? len : s_slot_cap;
	}
	memcpy(slot->buf, data, len);
	slot->len = len;
	slot->profile = profile;
	slot->jpeg_q = jpeg_q;
	slot->ts_ms = (uint32_t)(esp_timer_get_time()/1000ULL);
	s_latest = next; s_accepted++;
	return ESP_OK;
}

int opdi_cam_stream_copy_latest(uint8_t *out, size_t cap, uint8_t *out_q, opdi_cam_profile_t *out_profile, uint32_t *out_ts_ms){
	if (s_latest < 0) return 0;
	cam_stream_slot_t *slot = &s_slots[s_latest];
	if (!out){ return (int)slot->len; }
	if (cap < slot->len) return -(int)slot->len;
	memcpy(out, slot->buf, slot->len);
	if (out_q) *out_q = slot->jpeg_q;
	if (out_profile) *out_profile = slot->profile;
	if (out_ts_ms) *out_ts_ms = slot->ts_ms;
	s_served++;
	return (int)slot->len;
}

// Hook into telemetry periodic to update drop percentage.
extern void opdi_cam_adjust_stream_metrics(uint8_t fps_stream, uint8_t drop_pct);
__attribute__((weak)) void opdi_cam_stream_periodic_1s(void){
	// Compute drops vs accepted over last window - simplistic (resets every sec by caller if needed)
	static uint32_t last_acc=0, last_drop=0;
	uint32_t acc_delta = s_accepted - last_acc;
	uint32_t drop_delta = s_dropped - last_drop;
	last_acc = s_accepted; last_drop = s_dropped;
	uint8_t drop_pct = 0;
	if (acc_delta + drop_delta){ drop_pct = (uint8_t)((drop_delta * 100U) / (acc_delta + drop_delta)); }
	// fps_stream approximated as accepted frames in last second
	opdi_cam_adjust_stream_metrics((uint8_t)acc_delta, drop_pct);
}

// Stats accessor for governor (accepted - served backlog)
void opdi_cam_stream_stats(uint32_t *accepted, uint32_t *served, uint32_t *dropped){
	if (accepted) *accepted = s_accepted;
	if (served) *served = s_served;
	if (dropped) *dropped = s_dropped;
}
