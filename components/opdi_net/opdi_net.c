// Core public header
#include "opdi_net.h"

// IDF
#include "esp_log.h"
#include "sdkconfig.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h" // for IP4_ADDR macro
#include "esp_random.h" // for esp_random() used in AP SSID fallback
#include "mbedtls/sha1.h"

// Std
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// Internal constants & helpers ------------------------------------------------
static const char *TAG = "opdi_net";
static const char *NVS_NAMESPACE = "net";
// meta/version used inline (avoid unused warning if optimizer misses uses)
static const char *NVS_KEY_MRU = "mru"; // blob/array of hashes
static const char *NVS_KEY_AP_SSID = "ap/ssid"; // string
static const char *NVS_KEY_AP_CH = "ap/ch"; // u8

// Configuration derived from Kconfig
#define MAX_PROFILES CONFIG_OPDI_NET_MAX_PROFILES

// MRU list of SHA1(ssid) keys (20 max). We store as 20 * 20 hex? We'll keep raw 20-byte digest.
typedef struct {
    size_t count;
    uint8_t sha1[MAX_PROFILES][20];
} mru_list_t;

// Credential structure persisted (variable length PSK) - simple serialization for phase 1
typedef struct {
    char ssid[33];
    char psk[65];
    uint8_t auth; // wifi_auth_mode_t
    bool hidden;
    uint64_t last_ts; // esp_timer_get_time() snapshot (us). Phase1 simple.
    uint32_t success_count;
} stored_cred_t;

// Context structures forward declarations for callbacks
struct forget_ctx { const char *pid; uint8_t digest[20]; char ssid[33]; bool found; };

static bool forget_cb(const uint8_t d[20], const stored_cred_t *c, void *arg) {
    struct forget_ctx *cx = (struct forget_ctx*)arg;
    char tail[9]; snprintf(tail, sizeof(tail), "%02x%02x%02x%02x", d[16], d[17], d[18], d[19]);
    if (strncmp(cx->pid, tail, strlen(cx->pid))==0) {
        memcpy(cx->digest, d, 20);
        strlcpy(cx->ssid, c->ssid, sizeof(cx->ssid));
        cx->found = true;
        return false; // stop iteration
    }
    return true;
}

// State -----------------------------------------------------------------------
static opdi_net_state_t g_state = NET_INIT;
static mru_list_t g_mru; // guarded by esp_event loop context (single threaded modifications)
static uint32_t g_total_retries = 0; // counts attempts since boot or last successful connect
static uint64_t g_current_profile_start_ts = 0; // microseconds timestamp when current attempt began
static uint8_t g_current_profile_digest[20];
static bool g_have_current = false;
static bool g_bootstrap_attempted = false; // ensure we only try bootstrap once
static char g_last_ip[16] = ""; // cached dotted quad
static char g_last_gw[16] = ""; // cached gateway
static char g_ap_ssid[33] = "OPDI_SKPR-XXXX"; // persisted fallback AP SSID
static uint8_t g_ap_channel = CONFIG_OPDI_AP_CHANNEL;

// Metrics -------------------------------------------------------------------
static uint32_t g_metric_connect_attempts = 0;
static uint32_t g_metric_connect_success = 0;
static uint64_t g_metric_connect_time_accum_ms = 0; // sum of ms
static uint32_t g_metric_scan_count = 0;
// Hosted (C6) firmware version (captured if available)
static char g_hosted_fw_version[32] = "";

// Logs ring buffer (simple recent events) -----------------------------------
#define OPDI_NET_LOG_MAX 16
typedef struct { uint32_t ts_ms; char msg[48]; } net_log_t;
static net_log_t g_logs[OPDI_NET_LOG_MAX];
static size_t g_logs_head = 0; // next write position
static size_t g_logs_count = 0;

static void log_event(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char tmp[48]; vsnprintf(tmp, sizeof(tmp), fmt, ap); va_end(ap);
    net_log_t *slot = &g_logs[g_logs_head];
    slot->ts_ms = (uint32_t)(esp_timer_get_time()/1000ULL);
    strlcpy(slot->msg, tmp, sizeof(slot->msg));
    g_logs_head = (g_logs_head + 1) % OPDI_NET_LOG_MAX;
    if (g_logs_count < OPDI_NET_LOG_MAX) g_logs_count++;
}

// Timers (FreeRTOS timers or esp_timer) -> using esp_timer for lightweight periodic callback
static esp_timer_handle_t s_ap_retry_timer; // fires in AP mode to trigger STA retry
static esp_timer_handle_t s_ap_mac_check_timer; // short-lived timer to verify AP MAC becomes non-zero
static esp_timer_handle_t s_metrics_timer;      // periodic metrics emission

// Bootstrap scan state (scan-before-AP fallback requirement)
static bool g_bootstrap_scan_started = false;
static bool g_bootstrap_scan_done = false;
static char g_bootstrap_scan_summary[512]; // JSON-ish summary of top 16 BSSIDs

// Forward for AP MAC check scheduling
static void schedule_ap_mac_check(bool start);
static void schedule_metrics_timer(bool start);

// Forward declarations
static void enter_state(opdi_net_state_t st);
static esp_err_t load_profiles(void);
static void schedule_ap_retry_timer(bool start);
static void attempt_next_profile(const char *force_ssid);
static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data);
static void ip_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data);
static esp_err_t ensure_wifi(void);
static esp_err_t start_sta_mode(void);
static esp_err_t start_ap_mode(void);
static wifi_auth_mode_t map_auth_mode(uint8_t stored);
static void schedule_internal_scan(void);
static void handle_scan_done(void);

// Internal scan timer (to kick off scan a short delay after AP starts)
static esp_timer_handle_t s_internal_scan_timer;
static void internal_scan_timer_cb(void *arg){
    if (g_state != NET_AP_ACTIVE) return;
    // Use active scan with very short dwell to minimize driver stack usage in event task
    wifi_scan_config_t sc = {0};
    sc.show_hidden = true;
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE; // passive caused stack protection fault on sys_evt
    sc.channel = g_ap_channel; // single channel only
    // Configure minimal dwell time (values in ms) – guard with IDF struct layout
    sc.scan_time.active.min = 50; // 50ms
    sc.scan_time.active.max = 100; // 100ms
    esp_err_t r = esp_wifi_scan_start(&sc, false);
    ESP_LOGI(TAG, "internal scan start ch=%u r=%d", (unsigned)g_ap_channel, (int)r);
    if (r!=ESP_OK) log_event("scan_err"); else log_event("scan_start");
}
static void schedule_internal_scan(void){
    if (s_internal_scan_timer){ esp_timer_stop(s_internal_scan_timer); }
    const esp_timer_create_args_t args = { .callback=&internal_scan_timer_cb, .name="intscan" };
    if (!s_internal_scan_timer && esp_timer_create(&args, &s_internal_scan_timer)!=ESP_OK) return;
    // Fire once 2s after AP active
    esp_timer_start_once(s_internal_scan_timer, 2000000ULL);
}
static void handle_scan_done(void){
    // Reduce on-stack allocation size: make static & smaller to avoid exhausting sys_evt stack
    uint16_t ap_num=0; static wifi_ap_record_t recs[8];
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num > 8) ap_num = 8;
    esp_err_t gr = esp_wifi_scan_get_ap_records(&ap_num, recs);
    bool found=false; int rssi=0;
    if (gr==ESP_OK){
        for (int i=0;i<ap_num;i++) {
            if (strncmp((const char*)recs[i].ssid, g_ap_ssid, sizeof(recs[i].ssid))==0){ found=true; rssi=recs[i].rssi; break; }
        }
    }
    ESP_LOGI(TAG, "internal scan done (gr=%d aps=%u) found_self=%d rssi=%d", (int)gr, (unsigned)ap_num, found, rssi);
    log_event(found?"self_found":"self_miss");
}

// SHA1 implementation placeholder (Phase1: we can call mbedTLS) ----------------
static esp_err_t sha1_of_ssid(const char *ssid, uint8_t out[20]) {
    if (!ssid || !out) return ESP_ERR_INVALID_ARG;
    size_t l = strnlen(ssid, 32);
    if (l == 0) return ESP_ERR_INVALID_ARG;
    mbedtls_sha1_context ctx; mbedtls_sha1_init(&ctx); mbedtls_sha1_starts(&ctx);
    mbedtls_sha1_update(&ctx, (const unsigned char*)ssid, l);
    mbedtls_sha1_finish(&ctx, out);
    mbedtls_sha1_free(&ctx);
    return ESP_OK;
}

// Map persisted auth (uint8_t) to valid wifi_auth_mode_t; fall back to WPA2 if unknown
static wifi_auth_mode_t map_auth_mode(uint8_t stored){
    switch(stored){
    case WIFI_AUTH_OPEN:
    case WIFI_AUTH_WPA2_PSK:
#ifdef WIFI_AUTH_WPA3_PSK
    case WIFI_AUTH_WPA3_PSK:
#endif
#ifdef WIFI_AUTH_WPA2_WPA3_PSK
    case WIFI_AUTH_WPA2_WPA3_PSK:
#endif
        return (wifi_auth_mode_t)stored;
    default:
        return WIFI_AUTH_WPA2_PSK; // conservative default
    }
}

static bool is_placeholder_digest(const uint8_t d[20]) {
    for (int i=0;i<19;i++) if (d[i]!=0) return false; // placeholder had first 19 bytes zero
    return true; // (last byte = length) any length accepted
}

// Forward declarations for helpers used before definition
static esp_err_t save_mru(void);
static esp_err_t nvs_open_net(nvs_handle_t *h);

static esp_err_t set_meta_version(uint8_t v) {
    nvs_handle_t h; esp_err_t e = nvs_open_net(&h); if (e!=ESP_OK) { ESP_LOGE(TAG, "open meta err=%d", e); return e; }
    esp_err_t err = nvs_set_u8(h, "meta/version", v);
    if (err==ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static uint8_t get_meta_version(void) {
    nvs_handle_t h; if (nvs_open_net(&h)!=ESP_OK) return 0;
    uint8_t v=0; nvs_get_u8(h, "meta/version", &v); nvs_close(h); return v;
}

// Migration: rename any placeholder digest keys to real SHA1-based keys
static esp_err_t migrate_placeholder_digests(void) {
    uint8_t meta = get_meta_version();
    if (meta >= 1) return ESP_OK; // already migrated
    ESP_LOGI(TAG, "migration: scanning for placeholder credential digests");
    // We will collect mapping old->new to update MRU after rename.
    typedef struct { uint8_t old_d[20]; uint8_t new_d[20]; } map_t;
    map_t maps[MAX_PROFILES]; size_t map_count=0;
    // Iterate credentials
    nvs_handle_t h; esp_err_t eopen = nvs_open_net(&h); if (eopen!=ESP_OK) { ESP_LOGE(TAG, "migration open err=%d", eopen); return eopen; }
    nvs_iterator_t it = NULL; esp_err_t err = nvs_entry_find(NULL, NVS_NAMESPACE, NVS_TYPE_BLOB, &it);
    while (err == ESP_OK && it) {
        nvs_entry_info_t info; nvs_entry_info(it, &info);
        if (strncmp(info.key, "cred/", 5)==0 && strlen(info.key)==45) {
            uint8_t old_digest[20]={0};
            for (int i=0;i<20;i++){ unsigned int b=0; sscanf(&info.key[5+i*2], "%02x", &b); old_digest[i]=(uint8_t)b; }
            if (is_placeholder_digest(old_digest)) {
                stored_cred_t cred={0}; size_t sz=sizeof(cred);
                if (nvs_get_blob(h, info.key, &cred, &sz)==ESP_OK && sz==sizeof(cred)) {
                    uint8_t new_digest[20]; if (sha1_of_ssid(cred.ssid, new_digest)==ESP_OK && memcmp(new_digest, old_digest, 20)!=0) {
                        char new_key[6+40+1]; strcpy(new_key, "cred/");
                        for (int i=0;i<20;i++) { sprintf(&new_key[5+i*2], "%02x", new_digest[i]); }
                        new_key[45]='\0';
                        // Write new key if not existing
                        size_t dummy_sz = sizeof(cred); stored_cred_t tmp; bool exists=false;
                        if (nvs_get_blob(h, new_key, &tmp, &dummy_sz)==ESP_OK) exists=true;
                        if (!exists) {
                            if ((err = nvs_set_blob(h, new_key, &cred, sizeof(cred)))==ESP_OK && (err = nvs_erase_key(h, info.key))==ESP_OK && (err = nvs_commit(h))==ESP_OK) {
                                if (map_count < MAX_PROFILES) { memcpy(maps[map_count].old_d, old_digest,20); memcpy(maps[map_count].new_d, new_digest,20); map_count++; }
                                ESP_LOGI(TAG, "migrated profile '%s'", cred.ssid);
                            } else {
                                ESP_LOGW(TAG, "failed migrate key %s -> %s err=%d", info.key, new_key, err);
                            }
                        } else {
                            // Duplicate (same SSID) already migrated; just remove old
                            if ((err = nvs_erase_key(h, info.key))==ESP_OK && (err = nvs_commit(h))==ESP_OK) {
                                ESP_LOGI(TAG, "removed duplicate placeholder %s", info.key);
                            }
                        }
                    }
                }
            }
        }
        err = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);
    nvs_close(h);
    // Update MRU entries
    if (map_count) {
        for (size_t i=0;i<g_mru.count;i++) {
            for (size_t m=0;m<map_count;m++) {
                if (memcmp(g_mru.sha1[i], maps[m].old_d,20)==0) {
                    memcpy(g_mru.sha1[i], maps[m].new_d,20);
                    break;
                }
            }
        }
        save_mru();
    }
    set_meta_version(1);
    return ESP_OK;
}

// MRU utilities ---------------------------------------------------------------
static int mru_index_of(const uint8_t sha1[20]) {
    for (size_t i=0;i<g_mru.count;i++) {
        if (memcmp(g_mru.sha1[i], sha1, 20)==0) return (int)i;
    }
    return -1;
}

static void mru_promote_or_add(const uint8_t sha1[20]) {
    int idx = mru_index_of(sha1);
    if (idx == 0) return; // already MRU
    if (idx > 0) {
        uint8_t tmp[20];
        memcpy(tmp, sha1, 20);
        memmove(&g_mru.sha1[1], &g_mru.sha1[0], idx * 20);
        memcpy(g_mru.sha1[0], tmp, 20);
        return;
    }
    // Not present
    if (g_mru.count < MAX_PROFILES) {
        memmove(&g_mru.sha1[1], &g_mru.sha1[0], g_mru.count * 20);
        memcpy(g_mru.sha1[0], sha1, 20);
        g_mru.count++;
    } else {
        // shift down dropping LRU
        memmove(&g_mru.sha1[1], &g_mru.sha1[0], (MAX_PROFILES-1) * 20);
        memcpy(g_mru.sha1[0], sha1, 20);
    }
}

static void mru_remove(const uint8_t sha1[20]) {
    int idx = mru_index_of(sha1);
    if (idx < 0) return;
    if (idx < (int)g_mru.count-1) {
        memmove(&g_mru.sha1[idx], &g_mru.sha1[idx+1], (g_mru.count-idx-1)*20);
    }
    if (g_mru.count) g_mru.count--;
}

// Persistence -----------------------------------------------------------------
static esp_err_t nvs_open_net(nvs_handle_t *h) {
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, h);
    if (err == ESP_ERR_NVS_NOT_INITIALIZED) {
        // caller should have initialized flash; propagate
    }
    return err;
}

static esp_err_t save_mru(void) {
    nvs_handle_t h; esp_err_t e = nvs_open_net(&h); if (e!=ESP_OK) { ESP_LOGE(TAG, "open mru err=%d", e); return e; }
    esp_err_t err = nvs_set_blob(h, NVS_KEY_MRU, &g_mru, sizeof(size_t) + g_mru.count*20);
    if (err==ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t load_mru(void) {
    memset(&g_mru, 0, sizeof(g_mru));
    nvs_handle_t h; esp_err_t err = nvs_open_net(&h);
    if (err!=ESP_OK) return err;
    size_t required = sizeof(g_mru);
    err = nvs_get_blob(h, NVS_KEY_MRU, &g_mru, &required);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK; // treat as empty
    return err;
}

static esp_err_t save_profile(const stored_cred_t *cred) {
    uint8_t digest[20]; sha1_of_ssid(cred->ssid, digest);
    char key[6 + 40 + 1]; // 'cred/' + 40hex
    strcpy(key, "cred/");
    // quick hex using digest minimal uniqueness for placeholder
    for (int i=0;i<20;i++) sprintf(&key[5+i*2], "%02x", digest[i]);
    key[5+40] = '\0';
    nvs_handle_t h; esp_err_t e = nvs_open_net(&h); if (e!=ESP_OK) { ESP_LOGE(TAG, "open prof err=%d", e); return e; }
    esp_err_t err = nvs_set_blob(h, key, cred, sizeof(*cred));
    if (err==ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// Enumerate profiles: iterate over NVS entries with key prefix "cred/".
typedef bool (*profile_enum_cb)(const uint8_t digest[20], const stored_cred_t *cred, void *arg);

static esp_err_t foreach_profile(profile_enum_cb cb, void *arg) {
    nvs_handle_t h; esp_err_t e = nvs_open_net(&h); if (e!=ESP_OK) { ESP_LOGE(TAG, "open foreach err=%d", e); return e; }
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NULL, NVS_NAMESPACE, NVS_TYPE_BLOB, &it);
    while (err == ESP_OK && it) {
        nvs_entry_info_t info; nvs_entry_info(it, &info);
        if (strncmp(info.key, "cred/", 5) == 0) {
            // parse hex digest
            uint8_t digest[20] = {0};
            if (strlen(info.key) == 5 + 40) {
                for (int i=0;i<20;i++) {
                    unsigned int byte=0; sscanf(&info.key[5+i*2], "%02x", &byte); digest[i]=(uint8_t)byte; }
            }
            stored_cred_t cred = {0}; size_t sz = sizeof(cred);
            if (nvs_get_blob(h, info.key, &cred, &sz)==ESP_OK && sz==sizeof(cred)) {
                if (!cb(digest, &cred, arg)) break;
            }
        }
        err = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);
    nvs_close(h);
    return ESP_OK;
}

// Lookup a profile by SSID (loads full credential). Returns ESP_ERR_NOT_FOUND if absent.
static esp_err_t load_profile_by_ssid(const char *ssid, stored_cred_t *out, uint8_t out_digest[20]) {
    if (!ssid) return ESP_ERR_INVALID_ARG;
    uint8_t digest[20]; sha1_of_ssid(ssid, digest);
    if (out_digest) memcpy(out_digest, digest, 20);
    char key[6 + 40 + 1]; strcpy(key, "cred/");
    for (int i=0;i<20;i++) sprintf(&key[5+i*2], "%02x", digest[i]);
    key[5+40]='\0';
    nvs_handle_t h; esp_err_t e = nvs_open_net(&h); if (e!=ESP_OK) { ESP_LOGE(TAG, "open load prof err=%d", e); return e; }
    size_t sz = sizeof(stored_cred_t);
    esp_err_t err = nvs_get_blob(h, key, out, &sz);
    nvs_close(h);
    return err;
}

static esp_err_t erase_profile_by_ssid(const char *ssid) {
    uint8_t digest[20]; sha1_of_ssid(ssid, digest);
    char key[6 + 40 + 1]; strcpy(key, "cred/");
    for (int i=0;i<20;i++) sprintf(&key[5+i*2], "%02x", digest[i]);
    key[5+40]='\0';
    nvs_handle_t h; esp_err_t e = nvs_open_net(&h); if (e!=ESP_OK) { ESP_LOGE(TAG, "open erase prof err=%d", e); return e; }
    esp_err_t err = nvs_erase_key(h, key);
    if (err==ESP_ERR_NVS_NOT_FOUND) err = ESP_OK; // idempotent
    if (err==ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    // update MRU
    mru_remove(digest); save_mru();
    return err;
}

static esp_err_t load_profiles(void) {
    // Phase1 minimal: load MRU list only; individual creds loaded on demand.
    return load_mru();
}

// State management ------------------------------------------------------------
// Weak event emission hooks (can be overridden by WebSocket or MQTT integration component)
__attribute__((weak)) void opdi_net_emit_sta_connected(const char *ip, int rssi, const uint8_t bssid[6]) {
    (void)ip; (void)rssi; (void)bssid; ESP_LOGI(TAG, "emit sta_connected (stub)"); }
__attribute__((weak)) void opdi_net_emit_sta_disconnected(int reason) { (void)reason; ESP_LOGI(TAG, "emit sta_disconnected (stub)"); }
__attribute__((weak)) void opdi_net_emit_ap_active(const char *ssid, uint8_t channel) { (void)ssid; (void)channel; ESP_LOGI(TAG, "emit ap_active (stub)"); }

// LED pattern hooks (weak)
__attribute__((weak)) void opdi_led_set_green_solid(void){ ESP_LOGD(TAG, "green solid (stub)"); }
__attribute__((weak)) void opdi_led_set_green_blink1hz(void){ ESP_LOGD(TAG, "green 1Hz (stub)"); }
__attribute__((weak)) void opdi_led_set_green_off(void){ ESP_LOGD(TAG, "green off (stub)"); }
__attribute__((weak)) void opdi_led_set_red_blink2hz(void){ ESP_LOGD(TAG, "red 2Hz (stub)"); }
__attribute__((weak)) void opdi_led_set_red_off(void){ ESP_LOGD(TAG, "red off (stub)"); }

static void enter_state(opdi_net_state_t st) {
    if (g_state == st) return;
    ESP_LOGI(TAG, "state: %d -> %d", g_state, st);
    log_event("state %d->%d", g_state, st);
    g_state = st;
    // Emit metrics hook (weak) on every state transition
    __attribute__((weak)) void opdi_net_emit_metrics(void); // forward weak decl if not earlier
    if (&opdi_net_emit_metrics) opdi_net_emit_metrics();
    switch (st) {
        case NET_AP_ACTIVE:
            schedule_ap_retry_timer(true);
            #if !CONFIG_OPDI_NET_TESTING
            if (ensure_wifi()==ESP_OK) start_ap_mode();
            #endif
            // After starting AP, verify MAC assignment (some hosted races yield 00:00:.. initially)
            schedule_ap_mac_check(true);
            opdi_net_emit_ap_active(g_ap_ssid, g_ap_channel);
            opdi_led_set_green_blink1hz();
            opdi_led_set_red_off();
            schedule_internal_scan();
            break;
        case NET_STA_CONNECTED:
            schedule_ap_retry_timer(false);
            g_total_retries = 0;
            opdi_led_set_green_solid();
            opdi_led_set_red_off();
            break;
        default:
            // NET_INIT or NET_STA_CONNECT
            if (st == NET_STA_CONNECT) { 
                opdi_led_set_red_blink2hz(); opdi_led_set_green_off();
#if !CONFIG_OPDI_NET_TESTING
                if (ensure_wifi()==ESP_OK) start_sta_mode();
#endif
            }
            break;
    }
}

// Timer callback for AP periodic retry
static void ap_retry_timer_cb(void *arg) {
    if (g_state != NET_AP_ACTIVE) return;
    ESP_LOGI(TAG, "AP retry timer firing - attempting STA reconnect");
    enter_state(NET_STA_CONNECT);
    attempt_next_profile(NULL);
}

static void schedule_ap_retry_timer(bool start) {
    if (start) {
        if (!s_ap_retry_timer) {
            const esp_timer_create_args_t args = {
                .callback = &ap_retry_timer_cb,
                .arg = NULL,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "ap_retry"
            };
            if (esp_timer_create(&args, &s_ap_retry_timer)==ESP_OK) {
                esp_timer_start_periodic(s_ap_retry_timer, (uint64_t)CONFIG_OPDI_NET_RETRY_PERIOD_S * 1000000ULL);
            }
        } else {
            esp_timer_stop(s_ap_retry_timer);
            esp_timer_start_periodic(s_ap_retry_timer, (uint64_t)CONFIG_OPDI_NET_RETRY_PERIOD_S * 1000000ULL);
        }
    } else if (s_ap_retry_timer) {
        esp_timer_stop(s_ap_retry_timer);
    }
}

// AP MAC verification timer -------------------------------------------------
static void ap_mac_check_cb(void *arg){
    if (g_state != NET_AP_ACTIVE) { schedule_ap_mac_check(false); return; }
    static int attempts = 0;
    uint8_t mac[6]={0};
    esp_err_t r = esp_wifi_get_mac(WIFI_IF_AP, mac);
    bool zero = (r!=ESP_OK) || ((mac[0]|mac[1]|mac[2]|mac[3]|mac[4]|mac[5])==0);
    if (!zero) {
        ESP_LOGI(TAG, "AP MAC ready %02X:%02X:%02X:%02X:%02X:%02X (attempt %d)", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], attempts);
        log_event("ap_mac_ok");
        attempts = 0;
        schedule_ap_mac_check(false);
        return;
    }
    attempts++;
    ESP_LOGW(TAG, "AP MAC still zero (attempt %d) - restarting AP mode", attempts);
    log_event("ap_mac_zero");
    if (attempts <= 5) {
        start_ap_mode(); // re-run start sequence to attempt MAC assignment
    } else {
        ESP_LOGE(TAG, "AP MAC failed to become non-zero after %d attempts", attempts);
        log_event("ap_mac_fail");
        schedule_ap_mac_check(false);
        attempts = 0;
    }
}
static void schedule_ap_mac_check(bool start){
    if (start) {
        if (!s_ap_mac_check_timer) {
            const esp_timer_create_args_t args={ .callback=&ap_mac_check_cb, .name="ap_mac_chk" };
            if (esp_timer_create(&args, &s_ap_mac_check_timer)!=ESP_OK) return;
        } else {
            esp_timer_stop(s_ap_mac_check_timer);
        }
        // run every 1s
        esp_timer_start_periodic(s_ap_mac_check_timer, 1000000ULL);
    } else if (s_ap_mac_check_timer) {
        esp_timer_stop(s_ap_mac_check_timer);
    }
}

// Metrics periodic emission -------------------------------------------------
static void metrics_timer_cb(void *arg){
    (void)arg;
    __attribute__((weak)) void opdi_net_emit_metrics(void);
    if (&opdi_net_emit_metrics) opdi_net_emit_metrics();
}
static void schedule_metrics_timer(bool start){
    if (start){
        if (!s_metrics_timer){
            const esp_timer_create_args_t args={ .callback=&metrics_timer_cb, .name="net_metrics" };
            if (esp_timer_create(&args, &s_metrics_timer)!=ESP_OK) return;
        } else {
            esp_timer_stop(s_metrics_timer);
        }
        // every 30s
        esp_timer_start_periodic(s_metrics_timer, 30000000ULL);
    } else if (s_metrics_timer){
        esp_timer_stop(s_metrics_timer);
    }
}

// Connection attempt logic (placeholder)
static void attempt_next_profile(const char *force_ssid) {
#if CONFIG_OPDI_NET_TESTING
    // Skip real Wi-Fi operations; just simulate state transitions and metrics
    if (force_ssid && force_ssid[0]) {
        g_metric_connect_attempts++; g_total_retries++; log_event("attempt test");
        // Immediately simulate success for deterministic tests
        g_metric_connect_success++; g_metric_connect_time_accum_ms += 50; enter_state(NET_STA_CONNECTED);
        strlcpy(g_last_ip, "10.0.0.2", sizeof(g_last_ip)); strlcpy(g_last_gw, "10.0.0.1", sizeof(g_last_gw));
        opdi_net_emit_sta_connected(g_last_ip, -30, NULL);
        return;
    }
#endif
    if (force_ssid && force_ssid[0]) {
        ESP_LOGI(TAG, "Attempt direct connect to requested SSID (masked) total_retries=%u", g_total_retries);
        stored_cred_t cred; uint8_t digest[20];
        if (load_profile_by_ssid(force_ssid, &cred, digest)==ESP_OK) {
            mru_promote_or_add(digest); save_mru();
            memcpy(g_current_profile_digest, digest, 20); g_have_current=true; g_current_profile_start_ts = esp_timer_get_time();
            g_metric_connect_attempts++; log_event("attempt %s", "direct");
#if !CONFIG_OPDI_NET_TESTING
            // Configure and connect now that STA mode should be active
            wifi_config_t wcfg={0};
            strlcpy((char*)wcfg.sta.ssid, cred.ssid, sizeof(wcfg.sta.ssid));
            if (cred.psk[0]) strlcpy((char*)wcfg.sta.password, cred.psk, sizeof(wcfg.sta.password));
            // Auth / hidden handling
            wcfg.sta.threshold.authmode = map_auth_mode(cred.auth);
            if (cred.hidden){
                // Hidden SSID: rely on directed probe; FAST scan reduces dwell
                wcfg.sta.scan_method = WIFI_FAST_SCAN;
                log_event("hidden ssid");
            }
            esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_set_config(WIFI_IF_STA, &wcfg); esp_wifi_connect();
#endif
        } else {
            ESP_LOGW(TAG, "Requested SSID not found");
            log_event("ssid not found");
        }
        g_total_retries++;
    } else if (g_mru.count == 0) {
        // No stored profiles: attempt bootstrap SSID if configured and not yet tried
#if !CONFIG_OPDI_NET_TESTING
        #ifdef CONFIG_OPDI_NET_BOOTSTRAP_SSID
        if (!g_bootstrap_attempted && CONFIG_OPDI_NET_BOOTSTRAP_SSID[0]) {
            g_bootstrap_attempted = true;
            ESP_LOGI(TAG, "Bootstrap attempt SSID='%s'", CONFIG_OPDI_NET_BOOTSTRAP_SSID);
            wifi_config_t wcfg = {0};
            strlcpy((char*)wcfg.sta.ssid, CONFIG_OPDI_NET_BOOTSTRAP_SSID, sizeof(wcfg.sta.ssid));
            #ifdef CONFIG_OPDI_NET_BOOTSTRAP_PSK
            if (CONFIG_OPDI_NET_BOOTSTRAP_PSK[0]) strlcpy((char*)wcfg.sta.password, CONFIG_OPDI_NET_BOOTSTRAP_PSK, sizeof(wcfg.sta.password));
            #endif
            wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK; // conservative default; open if psk empty
            #ifdef CONFIG_OPDI_NET_BOOTSTRAP_PSK
            if (!CONFIG_OPDI_NET_BOOTSTRAP_PSK[0]) wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
            #endif
            esp_wifi_set_mode(WIFI_MODE_STA);
            esp_wifi_set_config(WIFI_IF_STA, &wcfg);
            g_metric_connect_attempts++; g_total_retries++;
            g_current_profile_start_ts = esp_timer_get_time();
            log_event("attempt bootstrap");
            esp_wifi_connect();
            return; // wait for result
        }
        #endif // CONFIG_OPDI_NET_BOOTSTRAP_SSID
#endif
    ESP_LOGW(TAG, "No profiles/Bootstrap failed - entering AP mode (total_retries=%u)", g_total_retries);
        log_event("no profiles");
        // Perform one active scan (scan-before-AP fallback requirement) before switching
        if (!g_bootstrap_scan_started && !g_bootstrap_scan_done) {
            wifi_scan_config_t sc={0};
            sc.show_hidden = true; sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
            sc.scan_time.active.min = 80; sc.scan_time.active.max = 120; // short dwell
            esp_err_t sr = esp_wifi_scan_start(&sc, false);
            if (sr==ESP_OK) { g_bootstrap_scan_started = true; ESP_LOGI(TAG, "Bootstrap scan started prior to AP fallback"); log_event("bs_scan_start"); return; }
            ESP_LOGW(TAG, "Bootstrap scan start failed (%d), proceeding to AP", (int)sr);
        }
        enter_state(NET_AP_ACTIVE);
        return;
    } else {
        // Check if current profile exceeded per-SSID timeout
        uint64_t now = esp_timer_get_time();
        if (g_have_current) {
            uint64_t elapsed_s = (now - g_current_profile_start_ts)/1000000ULL;
            if (elapsed_s < CONFIG_OPDI_NET_STA_TIMEOUT_S) {
                ESP_LOGI(TAG, "Continuing attempt (elapsed=%llu s < timeout)", (unsigned long long)elapsed_s);
                return; // still within timeout window; keep trying
            } else {
                ESP_LOGW(TAG, "Profile attempt timed out (%llu s) advancing MRU", (unsigned long long)elapsed_s);
                // rotate MRU: move current (index 0) to end implicitly by shifting others up and placing at end? Simpler: drop current
                mru_remove(g_current_profile_digest); save_mru();
                g_have_current=false;
                if (g_mru.count==0) { enter_state(NET_AP_ACTIVE); return; }
            }
        }
        // Attempt first MRU entry
    ESP_LOGI(TAG, "Attempting MRU[0] (hash tail=%02x%02x) total_retries=%u", g_mru.sha1[0][18], g_mru.sha1[0][19], g_total_retries);
        memcpy(g_current_profile_digest, g_mru.sha1[0], 20); g_have_current=true; g_current_profile_start_ts = now;
        g_metric_connect_attempts++; log_event("attempt mru");
        g_total_retries++;
#if !CONFIG_OPDI_NET_TESTING
        // Real Wi-Fi connect for MRU entry (placeholder minimal implementation)
        // Load credential to obtain SSID/PSK
        struct load_ctx { uint8_t target[20]; stored_cred_t cred; bool found; } lctx;
        memcpy(lctx.target, g_current_profile_digest,20); lctx.found=false;
        bool findcb(const uint8_t d[20], const stored_cred_t *c, void *arg){ struct load_ctx *lc=(struct load_ctx*)arg; if(memcmp(d,lc->target,20)==0){ lc->cred=*c; lc->found=true; return false;} return true; }
        foreach_profile((profile_enum_cb)findcb, &lctx);
        if (lctx.found) {
            wifi_config_t wcfg={0};
            strlcpy((char*)wcfg.sta.ssid, lctx.cred.ssid, sizeof(wcfg.sta.ssid));
            if (lctx.cred.psk[0]) strlcpy((char*)wcfg.sta.password, lctx.cred.psk, sizeof(wcfg.sta.password));
            wcfg.sta.threshold.authmode = map_auth_mode(lctx.cred.auth);
            if (lctx.cred.hidden){
                wcfg.sta.scan_method = WIFI_FAST_SCAN;
                log_event("hidden mru");
            }
            esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_set_config(WIFI_IF_STA, &wcfg); esp_wifi_connect();
        }
#endif
    }
    if (g_total_retries >= CONFIG_OPDI_NET_RETRIES) {
        ESP_LOGW(TAG, "Exceeded retries (%u) -> AP mode", g_total_retries);
        log_event("retries->AP");
        enter_state(NET_AP_ACTIVE);
    }
}

// Event handlers (skeleton) ---------------------------------------------------
static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (base != WIFI_EVENT) return;
    switch (id) {
        case WIFI_EVENT_SCAN_DONE:
            handle_scan_done();
            // If this was the bootstrap scan, collect top 16 SSIDs by RSSI and then enter AP mode
            if (g_bootstrap_scan_started && !g_bootstrap_scan_done) {
                g_bootstrap_scan_done = true;
                uint16_t total=0; esp_wifi_scan_get_ap_num(&total);
                if (total>0) {
                    uint16_t fetch = total; if (fetch > 64) fetch = 64; // upper bound
                    static wifi_ap_record_t recs[64];
                    if (esp_wifi_scan_get_ap_records(&fetch, recs)==ESP_OK) {
                        // Simple partial selection sort for top 16 by RSSI
                        uint16_t limit = fetch < 16 ? fetch : 16;
                        for (uint16_t i=0;i<limit;i++) {
                            uint16_t max_i = i;
                            for (uint16_t j=i+1;j<fetch;j++) if (recs[j].rssi > recs[max_i].rssi) max_i = j;
                            if (max_i != i) { wifi_ap_record_t tmp = recs[i]; recs[i]=recs[max_i]; recs[max_i]=tmp; }
                        }
                        // Build summary JSON-ish string
                        g_bootstrap_scan_summary[0]='\0';
                        size_t off=0; off += snprintf(g_bootstrap_scan_summary+off, sizeof(g_bootstrap_scan_summary)-off, "[");
                        for (uint16_t i=0;i<limit && off < sizeof(g_bootstrap_scan_summary)-16;i++) {
                            const char *ssid = (const char*)recs[i].ssid;
                            int n = snprintf(g_bootstrap_scan_summary+off, sizeof(g_bootstrap_scan_summary)-off,
                                             "%s{\"s\":\"%.*s\",\"r\":%d}",
                                             (i?",":""), 32, ssid, recs[i].rssi);
                            if (n<0) {
                                break;
                            }
                            off += n;
                        }
                        if (off < sizeof(g_bootstrap_scan_summary)-2) { g_bootstrap_scan_summary[off++]=']'; g_bootstrap_scan_summary[off]='\0'; }
                        ESP_LOGI(TAG, "Bootstrap scan top SSIDs: %s", g_bootstrap_scan_summary);
                        log_event("bs_scan_done");
                    }
                } else {
                    ESP_LOGW(TAG, "Bootstrap scan returned zero APs");
                    log_event("bs_scan_zero");
                }
                // Now transition to AP mode
                enter_state(NET_AP_ACTIVE);
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t*)data;
            int reason = ev ? ev->reason : 0;
            ESP_LOGW(TAG, "STA disconnected reason=%d", reason);
            log_event("disc %d", reason);
            opdi_net_emit_sta_disconnected(reason);
            // If bootstrap path (no stored profiles) and we haven't scanned yet, kick off scan then AP fallback
            if (g_bootstrap_attempted && g_mru.count==0 && !g_bootstrap_scan_started && !g_bootstrap_scan_done) {
                wifi_scan_config_t sc={0}; sc.show_hidden=true; sc.scan_type=WIFI_SCAN_TYPE_ACTIVE; sc.scan_time.active.min=80; sc.scan_time.active.max=120;
                if (esp_wifi_scan_start(&sc, false)==ESP_OK) { g_bootstrap_scan_started=true; ESP_LOGI(TAG, "Bootstrap disconnect -> scan-before-AP"); log_event("bs_scan_start"); return; }
            }
            if (g_state == NET_STA_CONNECTED || g_state == NET_STA_CONNECT) {
                if (g_total_retries < CONFIG_OPDI_NET_RETRIES) {
                    enter_state(NET_STA_CONNECT);
                    attempt_next_profile(NULL);
                } else {
                    ESP_LOGW(TAG, "Retry limit reached -> AP mode");
                    enter_state(NET_AP_ACTIVE);
                }
            }
            break; }
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "AP started");
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t*)data;
            if (ev) {
                ESP_LOGI(TAG, "AP station connected aid=%d mac=%02X:%02X:%02X:%02X:%02X:%02X", ev->aid, ev->mac[0],ev->mac[1],ev->mac[2],ev->mac[3],ev->mac[4],ev->mac[5]);
                log_event("ap_sta_conn");
            } else {
                ESP_LOGI(TAG, "AP station connected (no data)");
            }
            break; }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *ev = (wifi_event_ap_stadisconnected_t*)data;
            if (ev) {
                ESP_LOGI(TAG, "AP station disconnected aid=%d mac=%02X:%02X:%02X:%02X:%02X:%02X reason=%d", ev->aid, ev->mac[0],ev->mac[1],ev->mac[2],ev->mac[3],ev->mac[4],ev->mac[5], (int)ev->reason);
                log_event("ap_sta_disc");
            } else {
                ESP_LOGI(TAG, "AP station disconnected (no data)");
            }
            break; }
        default:
            break;
    }
}

static void ip_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Got IP");
        log_event("got ip");
        enter_state(NET_STA_CONNECTED);
        // Update success_count for active profile digest (if tracked)
        if (g_have_current) {
            // Need to load profile by digest - implement quick inline search by iterating
            struct upd_ctx { uint8_t target[20]; bool done; } uctx; memcpy(uctx.target, g_current_profile_digest,20); uctx.done=false;
            // Callback updates matching cred
            bool cbfn(const uint8_t d[20], const stored_cred_t *cred, void *arg){
                struct upd_ctx *c = (struct upd_ctx*)arg; if (memcmp(d,c->target,20)!=0) return true; // continue
                stored_cred_t mod = *cred; mod.success_count++; mod.last_ts = esp_timer_get_time(); save_profile(&mod); c->done=true; return false; }
            foreach_profile((profile_enum_cb)cbfn, &uctx);
        }
        g_total_retries = 0;
        // Query RSSI/BSSID
        // Extract IP from event structure if available
        if (data) {
            // ip_event_got_ip_t defined in esp_netif_ip_addr.h via ip_event.h
            const ip_event_got_ip_t *ev = (const ip_event_got_ip_t*)data;
            esp_ip4_addr_t ip = ev->ip_info.ip;
            snprintf(g_last_ip, sizeof(g_last_ip), IPSTR, IP2STR(&ip));
            esp_ip4_addr_t gw = ev->ip_info.gw;
            snprintf(g_last_gw, sizeof(g_last_gw), IPSTR, IP2STR(&gw));
        }
        wifi_ap_record_t ap; int rssi=0; uint8_t bssid_local[6]={0}; const uint8_t *bptr=NULL;
        if (esp_wifi_sta_get_ap_info(&ap)==ESP_OK) { rssi=ap.rssi; memcpy(bssid_local, ap.bssid,6); bptr=bssid_local; }
        opdi_net_emit_sta_connected(g_last_ip[0]?g_last_ip:"0.0.0.0", rssi, bptr);
        // Metrics success bookkeeping
        g_metric_connect_success++;
        if (g_have_current && g_current_profile_start_ts) {
            uint64_t elapsed_ms = (esp_timer_get_time() - g_current_profile_start_ts)/1000ULL;
            g_metric_connect_time_accum_ms += elapsed_ms;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_AP_STAIPASSIGNED) {
        ip_event_ap_staipassigned_t *ev = (ip_event_ap_staipassigned_t*)data;
        if (ev) {
            ESP_LOGI(TAG, "DHCP lease assigned to station: " IPSTR, IP2STR(&ev->ip));
            log_event("dhcp %u.%u.%u.%u", IP2STR(&ev->ip));
        } else {
            ESP_LOGI(TAG, "DHCP lease event (no data)");
        }
    }
}

// Public API ------------------------------------------------------------------
esp_err_t opdi_net_init(void) {
    ESP_LOGI(TAG, "init (ESP-Hosted, country=%s)", CONFIG_OPDI_COUNTRY_CODE);
    // NVS init (idempotent)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    {
        esp_err_t e = load_profiles(); if (e!=ESP_OK) { ESP_LOGE(TAG, "load profiles err=%d", e); return e; }
    }
    migrate_placeholder_digests();
    // Load AP config if exists
    nvs_handle_t h_ap; if (nvs_open_net(&h_ap)==ESP_OK) {
        size_t sl = sizeof(g_ap_ssid); nvs_get_str(h_ap, NVS_KEY_AP_SSID, g_ap_ssid, &sl);
        uint8_t ch=0; if (nvs_get_u8(h_ap, NVS_KEY_AP_CH, &ch)==ESP_OK) {
            if (ch>=1 && ch<=13) g_ap_channel = ch;
        }
        nvs_close(h_ap);
    }
    // Register events & netifs
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // Initialize netif (idempotent)
    static bool netif_inited=false; if(!netif_inited){ esp_netif_init(); netif_inited=true; }
    // Netifs: In hosted mode, the underlying wifi init may already have created default netifs.
    // Creating them twice leads to LWIP assert (netif already added). Guard with static flag AND
    // runtime check: esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") etc. If they exist skip creation.
    static bool netif_created=false; if(!netif_created){
        // In ESP-Hosted (esp_wifi_remote) the remote driver layer will create the default
        // WIFI_STA_DEF / WIFI_AP_DEF netifs asynchronously after the transport is up.
        // Creating them here races and can trigger lwIP assert (netif already added).
        // Just probe and log; never create explicitly in hosted mode.
        esp_netif_t *sta_h = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_t *ap_h  = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (sta_h || ap_h) {
            ESP_LOGI(TAG, "netif pre-existing (sta=%p ap=%p) - hosted mode skip creation", (void*)sta_h, (void*)ap_h);
        } else {
            ESP_LOGI(TAG, "netif not yet present - will rely on esp_wifi_remote to create later");
        }
        netif_created=true;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL);
    // Start hosted link (weak symbol)
    extern void opdi_net_hosted_start(void);
    opdi_net_hosted_start();
    // Run simple communication test (P4 -> C6) capturing STA/AP MAC & (if available) firmware version
    do {
        uint8_t mac_sta[6]={0}; uint8_t mac_ap[6]={0};
        esp_err_t msta = esp_wifi_get_mac(WIFI_IF_STA, mac_sta);
        esp_err_t map = esp_wifi_get_mac(WIFI_IF_AP, mac_ap);
        const char *fw = NULL;
        #ifdef ESP_WIFI_IS_VENDOR_IE_ENABLED
        #ifdef CONFIG_IDF_TARGET_ESP32P4
        #ifdef CONFIG_ESP_WIFI_SHOW_AP_INFO
        extern const char *esp_wifi_get_fw_version(void);
        fw = esp_wifi_get_fw_version();
        #endif
        #endif
        #endif
        if (msta==ESP_OK) {
            ESP_LOGI(TAG, "comm_test: STA MAC %02X:%02X:%02X:%02X:%02X:%02X", mac_sta[0],mac_sta[1],mac_sta[2],mac_sta[3],mac_sta[4],mac_sta[5]);
        } else {
            ESP_LOGW(TAG, "comm_test: STA MAC read err=%d", (int)msta);
        }
        if (map==ESP_OK) {
            ESP_LOGI(TAG, "comm_test: AP  MAC %02X:%02X:%02X:%02X:%02X:%02X", mac_ap[0],mac_ap[1],mac_ap[2],mac_ap[3],mac_ap[4],mac_ap[5]);
        } else {
            ESP_LOGW(TAG, "comm_test: AP MAC read err=%d", (int)map);
        }
        if (fw && fw[0]) ESP_LOGI(TAG, "comm_test: FW version '%s'", fw);
        wifi_country_t cc; if (esp_wifi_get_country(&cc)==ESP_OK){
            ESP_LOGI(TAG, "comm_test: country=%s schan=%d nchan=%d policy=%d", cc.cc, cc.schan, cc.nchan, cc.policy);
        }
        ESP_LOGI(TAG, "comm_test: result=%s", (msta==ESP_OK)?"OK":"PARTIAL");
    } while(0);
    // Attempt to log hosted (C6) firmware version if API available
    #ifdef ESP_WIFI_IS_VENDOR_IE_ENABLED // Use an existing Wi-Fi define as proxy that wifi headers present
    #ifdef CONFIG_IDF_TARGET_ESP32P4
    // Attempt firmware version API only if declared
    #ifdef CONFIG_ESP_WIFI_SHOW_AP_INFO // heuristic; adjust if actual macro differs
    extern const char *esp_wifi_get_fw_version(void);
    const char *fw = esp_wifi_get_fw_version();
    if (fw && fw[0]) { ESP_LOGI(TAG, "hosted fw version: %s", fw); strlcpy(g_hosted_fw_version, fw, sizeof(g_hosted_fw_version)); }
    else { ESP_LOGW(TAG, "hosted fw version: (empty)"); }
    #else
    ESP_LOGW(TAG, "hosted fw version API not enabled in this build");
    #endif
    #endif
    #endif
    // Start periodic metrics timer
    schedule_metrics_timer(true);
    enter_state(NET_STA_CONNECT);
    attempt_next_profile(NULL);
    return ESP_OK;
}

esp_err_t opdi_net_add_profile(const opdi_net_profile_t *p) {
    if (!p || p->ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (strlen(p->ssid) > 32 || strlen(p->psk) > 64) return ESP_ERR_INVALID_ARG;
    stored_cred_t cred = {0};
    memcpy(cred.ssid, p->ssid, sizeof(cred.ssid)-1);
    memcpy(cred.psk, p->psk, sizeof(cred.psk)-1);
    cred.auth = p->auth;
    cred.hidden = p->hidden;
    cred.last_ts = esp_timer_get_time();
    cred.success_count = 0; // updated on success
    {
        esp_err_t e = save_profile(&cred); if (e!=ESP_OK) { ESP_LOGE(TAG, "save profile err=%d", e); return e; }
    }
    uint8_t digest[20]; sha1_of_ssid(p->ssid, digest);
    mru_promote_or_add(digest);
    save_mru();
    ESP_LOGI(TAG, "profile added (hidden=%d)", p->hidden);
    // Disable further bootstrap attempts now that a real profile exists
    if (!g_bootstrap_attempted) { g_bootstrap_attempted = true; }
    log_event("bootstrap disabled profile_saved");
    return ESP_OK;
}

esp_err_t opdi_net_forget(const char *ssid) {
    if (!ssid) return ESP_ERR_INVALID_ARG;
    {
        esp_err_t e = erase_profile_by_ssid(ssid); if (e!=ESP_OK) { ESP_LOGE(TAG, "erase profile err=%d", e); return e; }
    }
    ESP_LOGI(TAG, "profile forgotten");
    return ESP_OK;
}

esp_err_t opdi_net_connect(const char *ssid_or_null) {
    enter_state(NET_STA_CONNECT);
    attempt_next_profile(ssid_or_null);
#if CONFIG_OPDI_NET_TESTING
    if (!ssid_or_null) {
        // Auto simulate connection success for MRU tests (if any attempts already made)
        if (g_state == NET_STA_CONNECT) {
            g_metric_connect_attempts++; g_metric_connect_success++; g_metric_connect_time_accum_ms += 42; enter_state(NET_STA_CONNECTED);
            strlcpy(g_last_ip, "10.0.0.3", sizeof(g_last_ip)); strlcpy(g_last_gw, "10.0.0.1", sizeof(g_last_gw));
            opdi_net_emit_sta_connected(g_last_ip, -28, NULL);
        }
    }
#endif
    return ESP_OK;
}

opdi_net_state_t opdi_net_get_state(void) { return g_state; }

// Internal enumeration exposure for REST (not in public header yet) -------------------------
typedef struct { char *buf; size_t cap; size_t len; } json_buf_t;

static bool enum_profiles_json_cb(const uint8_t digest[20], const stored_cred_t *cred, void *arg) {
    json_buf_t *jb = (json_buf_t*)arg;
    if (jb->len + 96 >= jb->cap) return false; // stop if near capacity
    // mask: use last 2 bytes of digest
    char entry[160];
    snprintf(entry, sizeof(entry),
             "{\"id\":\"%02x%02x%02x%02x...\",\"ssid_len\":%u,\"hidden\":%s,\"success\":%u},",
             digest[16], digest[17], digest[18], digest[19], (unsigned)strlen(cred->ssid), cred->hidden?"true":"false", (unsigned)cred->success_count);
    size_t el = strlen(entry);
    if (jb->len + el >= jb->cap) return false;
    memcpy(jb->buf + jb->len, entry, el); jb->len += el; jb->buf[jb->len] = '\0';
    return true;
}

size_t opdi_net_profiles_serialize(char *out, size_t out_cap) {
    if (!out || out_cap < 3) return 0;
    json_buf_t jb = { .buf = out, .cap = out_cap, .len = 0 };
    out[0]='['; out[1]='\0'; jb.len=1;
    foreach_profile(enum_profiles_json_cb, &jb);
    if (jb.len>1 && out[jb.len-1]==',') { out[jb.len-1] = ']'; out[jb.len]='\0'; }
    else { out[jb.len++] = ']'; out[jb.len]='\0'; }
    return jb.len;
}

esp_err_t opdi_net_forget_hash_tail(const char *partial_id) {
    if (!partial_id) return ESP_ERR_INVALID_ARG;
    // brute force iterate to find match on first 4 displayed bytes (digest[16..19])
    struct forget_ctx ctx = { .pid = partial_id, .found = false };
    foreach_profile(forget_cb, &ctx);
    if (!ctx.found) return ESP_ERR_NOT_FOUND;
    return opdi_net_forget(ctx.ssid);
}

// AP config persistence -----------------------------------------------------
esp_err_t opdi_net_ap_set(const char *ssid, uint8_t channel) {
    if (ssid && ssid[0]) {
        size_t l = strnlen(ssid, 32); if (l==0) return ESP_ERR_INVALID_ARG;
        memcpy(g_ap_ssid, ssid, l); g_ap_ssid[l]='\0';
    }
    if (channel>=1 && channel<=13) g_ap_channel = channel;
    nvs_handle_t h; esp_err_t e = nvs_open_net(&h); if (e!=ESP_OK) { ESP_LOGE(TAG, "open ap set err=%d", e); return e; }
    esp_err_t err = nvs_set_str(h, NVS_KEY_AP_SSID, g_ap_ssid);
    if (err==ESP_OK) err = nvs_set_u8(h, NVS_KEY_AP_CH, g_ap_channel);
    if (err==ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void opdi_net_ap_get(char *out_ssid, size_t len, uint8_t *out_channel) {
    if (out_ssid && len) strlcpy(out_ssid, g_ap_ssid, len);
    if (out_channel) *out_channel = g_ap_channel;
}

void opdi_net_get_metrics(opdi_net_metrics_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->connect_attempts = g_metric_connect_attempts;
    out->connects_success = g_metric_connect_success;
    if (g_metric_connect_success) {
        out->avg_connect_time_ms = (uint32_t)(g_metric_connect_time_accum_ms / g_metric_connect_success);
    }
    out->scan_count = g_metric_scan_count;
    out->current_retries = g_total_retries;
}

// Accessors for cached IP/GW (avoid exposing static symbols directly)
const char *opdi_net_ip_cached(void){ return g_last_ip; }
const char *opdi_net_gw_cached(void){ return g_last_gw; }

// Hosted FW version accessor
const char *opdi_net_hosted_fw_version(void){ return g_hosted_fw_version; }

size_t opdi_net_logs_serialize(char *out, size_t cap) {
    if (!out || cap < 4) return 0;
    size_t len = 0; out[len++]='['; out[len]='\0';
    // oldest first
    if (g_logs_count == 0) { out[len++]=']'; out[len]='\0'; return len; }
    size_t start = (g_logs_head + OPDI_NET_LOG_MAX - g_logs_count) % OPDI_NET_LOG_MAX;
    for (size_t i=0;i<g_logs_count;i++) {
        size_t idx = (start + i) % OPDI_NET_LOG_MAX;
        char entry[96];
    int n = snprintf(entry, sizeof(entry), "{\"t\":%lu,\"m\":\"%s\"}", (unsigned long)g_logs[idx].ts_ms, g_logs[idx].msg);
        if (n <= 0) continue;
        if (len + (size_t)n + 2 >= cap) break; // stop
        memcpy(out+len, entry, n); len += n; out[len]='\0';
        if (i != g_logs_count-1) { out[len++]=','; out[len]='\0'; }
    }
    if (len < cap-1) { out[len++]=']'; out[len]='\0'; }
    return len;
}

// Weak hook for scan accounting so we can increment internal counter when REST scan occurs
__attribute__((weak)) void opdi_net_internal_scan_account(void) { g_metric_scan_count++; log_event("scan"); }

// Wi-Fi helpers --------------------------------------------------------------
#if !CONFIG_OPDI_NET_TESTING
static esp_err_t ensure_wifi(void){
    static bool inited=false; static bool started=false;
    if (!inited){
        esp_err_t err = esp_wifi_init(&(wifi_init_config_t)WIFI_INIT_CONFIG_DEFAULT());
        if (err!=ESP_OK) return err;
        inited=true;
    }
    if (!started){
        esp_err_t err = esp_wifi_start(); if (err!=ESP_OK) return err; started=true; }
    // Always attempt to set country (harmless if repeated)
    esp_wifi_set_country_code(CONFIG_OPDI_COUNTRY_CODE, true);
    return ESP_OK;
}
static esp_err_t start_sta_mode(void){
    // Only set mode; connection happens in attempt logic
    return esp_wifi_set_mode(WIFI_MODE_STA);
}
static esp_err_t start_ap_mode(void){
    // Force channel override to 6 as requested (persist so next boot uses it)
    if (g_ap_channel != 6) { g_ap_channel = 6; opdi_net_ap_set(g_ap_ssid, g_ap_channel); }
    bool generated=false;
    // Regenerate SSID if placeholder present
    if (strstr(g_ap_ssid, "-XXXX") != NULL) {
        uint8_t mac_sta[6]={0};
        esp_err_t mac_res = esp_wifi_get_mac(WIFI_IF_STA, mac_sta);
        bool mac_valid = (mac_res==ESP_OK) && !(mac_sta[0]==0 && mac_sta[1]==0 && mac_sta[2]==0 && mac_sta[3]==0 && mac_sta[4]==0 && mac_sta[5]==0);
        if (mac_valid) {
            char new_ssid[33];
            int n = snprintf(new_ssid, sizeof(new_ssid), "OPDI_SKPR-%02X%02X", mac_sta[4], mac_sta[5]);
            if (n>0 && (size_t)n < sizeof(new_ssid)) { strlcpy(g_ap_ssid, new_ssid, sizeof(g_ap_ssid)); generated=true; }
        } else {
            // Fallback: stable-ish pseudo-random. Combine two esp_random calls to reduce chance of all zeros early.
            uint32_t r = esp_random() ^ (esp_random()<<11);
            char new_ssid[33];
            int n = snprintf(new_ssid, sizeof(new_ssid), "OPDI_SKPR-%04X", (unsigned)(r & 0xFFFF));
            if (n>0 && (size_t)n < sizeof(new_ssid)) { strlcpy(g_ap_ssid, new_ssid, sizeof(g_ap_ssid)); generated=true; }
            ESP_LOGW(TAG, "STA MAC invalid (zero). Using random suffix (r=0x%08X)", r);
        }
        if (generated) {
            // Persist so it remains stable across reboots
            opdi_net_ap_set(g_ap_ssid, g_ap_channel);
        }
    }

    // Prepare AP config now; will set MAC after mode but before start
    wifi_config_t ap={0};
    strlcpy((char*)ap.ap.ssid, g_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen((const char*)ap.ap.ssid);
    ap.ap.channel = g_ap_channel;
    ap.ap.max_connection = 4;
    ap.ap.authmode = CONFIG_OPDI_AP_OPEN ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_WPA2_PSK;
    ap.ap.ssid_hidden = 0; // ensure visible
    ap.ap.beacon_interval = 100; // explicit

    // Stop if running (ignore error), switch to AP mode
    esp_err_t r_stop = esp_wifi_stop();
    // Use APSTA so we can perform passive scans while AP active
    esp_err_t r_mode = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (r_mode!=ESP_OK) { ESP_LOGE(TAG, "AP set_mode failed %d", (int)r_mode); return r_mode; }

    // Ensure non-zero locally administered AP MAC BEFORE start
    uint8_t cur_mac[6]={0};
    esp_err_t gm = esp_wifi_get_mac(WIFI_IF_AP, cur_mac);
    bool mac_zero = (gm==ESP_OK) ? ((cur_mac[0]|cur_mac[1]|cur_mac[2]|cur_mac[3]|cur_mac[4]|cur_mac[5])==0) : true;
    if (mac_zero) {
        uint32_t r1 = esp_random();
        uint32_t r2 = esp_random();
        uint8_t newmac[6];
        newmac[0] = 0x02; // locally administered unicast
        newmac[1] = (uint8_t)(r1 & 0xFF);
        newmac[2] = (uint8_t)((r1>>8) & 0xFF);
        newmac[3] = (uint8_t)((r1>>16) & 0xFF);
        newmac[4] = (uint8_t)(r2 & 0xFF);
        newmac[5] = (uint8_t)((r2>>8) & 0xFF);
        esp_err_t sm = esp_wifi_set_mac(WIFI_IF_AP, newmac);
        ESP_LOGW(TAG, "Assigning local AP MAC result=%d new=%02X:%02X:%02X:%02X:%02X:%02X (gm=%d)", (int)sm, newmac[0],newmac[1],newmac[2],newmac[3],newmac[4],newmac[5], (int)gm);
    } else {
        ESP_LOGI(TAG, "Using existing AP MAC %02X:%02X:%02X:%02X:%02X:%02X", cur_mac[0],cur_mac[1],cur_mac[2],cur_mac[3],cur_mac[4],cur_mac[5]);
    }

    esp_err_t r_cfg  = esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_set_country_code(CONFIG_OPDI_COUNTRY_CODE, true);
    // Explicit AP netif IP configuration & DHCP server restart for hosted mode reliability
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        ESP_LOGI(TAG, "AP netif present before start (%p) - configuring IP/DHCP", (void*)ap_netif);
        // Stop DHCP server if running
        esp_netif_dhcps_stop(ap_netif);
        esp_netif_ip_info_t ipi; memset(&ipi,0,sizeof(ipi));
        IP4_ADDR(&ipi.ip, 192,168,4,1);
        IP4_ADDR(&ipi.gw, 192,168,4,1);
        IP4_ADDR(&ipi.netmask, 255,255,255,0);
        if (esp_netif_set_ip_info(ap_netif, &ipi)==ESP_OK){
            ESP_LOGI(TAG, "AP IP set to " IPSTR, IP2STR(&ipi.ip));
        } else {
            ESP_LOGW(TAG, "Failed to set AP IP info");
        }
        // Configure DHCP lease range if API available
        #ifdef ESP_NETIF_DHCP_LEASE
        esp_netif_dhcps_lease_t lease = {0};
        IP4_ADDR(&lease.enable, 1,0,0,0); // enable flag usage varies; guard compile
        IP4_ADDR(&lease.start_ip,192,168,4,2);
        IP4_ADDR(&lease.end_ip,192,168,4,20);
        if (esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS, &lease, sizeof(lease))==ESP_OK){
            ESP_LOGI(TAG, "AP DHCP lease range set 192.168.4.2-192.168.4.20");
        }
        #endif
        if (esp_netif_dhcps_start(ap_netif)==ESP_OK) {
            ESP_LOGI(TAG, "AP DHCP server started");
        } else {
            ESP_LOGW(TAG, "AP DHCP server start failed");
        }
    } else {
        ESP_LOGW(TAG, "AP netif handle not found prior to start");
    }
    esp_err_t r_start= esp_wifi_start();
    uint8_t final_mac[6]={0}; esp_wifi_get_mac(WIFI_IF_AP, final_mac);
    ESP_LOGI(TAG, "AP start sequence: stop=%d mode=%d cfg=%d start=%d ssid='%s' ch=%u mac=%02X:%02X:%02X:%02X:%02X:%02X", (int)r_stop, (int)r_mode, (int)r_cfg, (int)r_start, g_ap_ssid, (unsigned)g_ap_channel, final_mac[0],final_mac[1],final_mac[2],final_mac[3],final_mac[4],final_mac[5]);
    if (r_cfg!=ESP_OK) return r_cfg;
    if (r_start!=ESP_OK) return r_start;
    return ESP_OK;
}
#else
static esp_err_t ensure_wifi(void){ return ESP_OK; }
static esp_err_t start_sta_mode(void){ return ESP_OK; }
static esp_err_t start_ap_mode(void){ return ESP_OK; }
#endif

#ifdef CONFIG_OPDI_NET_TESTING
// Testing helpers ----------------------------------------------------------
void opdi_net_test_force_state(opdi_net_state_t st){ enter_state(st); }
void opdi_net_test_simulate_sta_connected(const char *ip, const char *gw, int rssi){
    if (ip) strlcpy(g_last_ip, ip, sizeof(g_last_ip));
    if (gw) strlcpy(g_last_gw, gw, sizeof(g_last_gw));
    // emulate metrics increment
    g_metric_connect_attempts++;
    g_metric_connect_success++;
    g_metric_connect_time_accum_ms += 100; // arbitrary small value for averaging
    enter_state(NET_STA_CONNECTED);
    uint8_t dummy_bssid[6]={0};
    opdi_net_emit_sta_connected(g_last_ip[0]?g_last_ip:"0.0.0.0", rssi, dummy_bssid);
}
#endif

