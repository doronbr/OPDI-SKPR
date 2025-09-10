# Networking (ESP-Hosted)

Current implementation (Phase 1 complete + incremental enhancements) provides a functional Wi‑Fi connection manager aligned with the design spec: profile persistence, MRU ordering, retry→AP fallback with periodic reconnect attempts, active scan REST endpoint, hash‑tail based profile deletion, LED pattern hooks, WebSocket event broadcast, success counters, AP config persistence, real SHA1 digests with migration, and enriched status (IP/RSSI). Remaining items focus on richer UI integration, security hardening, and auxiliary feature APIs (camera/audio/system/OTA).

## Architecture Overview
* Host MCU: **ESP32-P4** runs application logic + lwIP stack.
* Network Processor: **ESP32-C6** provides Wi‑Fi (ESP‑Hosted over SPI default: `CONFIG_OPDI_HOSTED_BUS="spi"`).
* Region: **IL** – channel usage & scans must respect Israeli regulatory domain (`CONFIG_OPDI_COUNTRY_CODE`).
* Fallback Provisioning AP: Open SSID `OPDI_SKPR-XXXX` (XXXX = last 4 hex of STA MAC), default channel `CONFIG_OPDI_AP_CHANNEL`.
* Profiles: Up to `CONFIG_OPDI_NET_MAX_PROFILES` (20) stored in MRU order; each includes SSID (masked in logs), PSK (not logged), auth mode, hidden flag, last success timestamp, success count. Profiles are keyed by `cred/<sha1(ssid)>` using real SHA1 (mbedTLS) with one‑time migration for legacy placeholder digests.

## State Machine
States:
`INIT` → `STA_CONNECT` → (`STA_CONNECTED` | `AP_ACTIVE`).

* `INIT`: NVS init, load MRU list, start ESP‑Hosted link (stub), register Wi‑Fi/IP events.
* `STA_CONNECT`: Iterate MRU profiles attempting connection. Each MRU[0] attempt has a per‑SSID timeout (`CONFIG_OPDI_NET_STA_TIMEOUT_S`); on timeout that profile is dropped from MRU (Phase1 simplification) and the next is attempted. Hidden profiles direct connect flow still placeholder.
* `STA_CONNECTED`: Successful station connection (IP event). Resets retry counter, stops AP retry timer.
* `AP_ACTIVE`: Provisioning AP active. Periodic timer (`CONFIG_OPDI_NET_RETRY_PERIOD_S`) triggers re‑attempt of STA connections.

Transition rules adhere to the requirements (retry threshold `CONFIG_OPDI_NET_RETRIES` -> AP mode, periodic retries from AP mode, reconnect on disconnect events).

## Persistence (Phase 1)
NVS Namespace: `net`.
* `mru` blob: internal structure storing up to 20 SHA1 digests (real SHA1 now in use; migration updates legacy placeholder entries in place and rewrites MRU accordingly, meta version flag set to 1).
* `cred/<sha1(ssid)>` blobs: `stored_cred_t` structure contains profile data (hidden, timestamps, counters). Success counter & `last_ts` increment on successful station connect (IP acquired).

Security Note: PSK stored plaintext pending flash encryption enable (Phase 2). Logs never print PSK; SSID masked to hash tail in some messages.

## Timers
* AP retry timer (esp_timer) fires every `CONFIG_OPDI_NET_RETRY_PERIOD_S` when in `AP_ACTIVE` to re-enter `STA_CONNECT`.
* Per‑SSID attempt tracking timestamps used to enforce `CONFIG_OPDI_NET_STA_TIMEOUT_S`.

## REST API
| Method | Path | Description | Status |
|--------|------|-------------|--------|
| GET | /api/v1/net/sta/status | `{state, ip?, rssi?}` | Implemented |
| GET | /api/v1/net/sta/profiles | List profiles `[{id,ssid_len,hidden,success}]` (SSID masked) | Implemented |
| POST | /api/v1/net/sta/profiles | Add/update profile `{ssid, psk?, hidden?, auth?}` (naive parse) | Implemented |
| DELETE | /api/v1/net/sta/profiles/{hashTail}` | Remove by last 4 bytes of SHA1 hex (hash tail) | Implemented |
| POST | /api/v1/net/sta/connect | Trigger connect to specific SSID or MRU when body empty | Implemented |
| GET | /api/v1/net/scan | Active scan results `[{ssid,bssid,rssi,auth,ch}]` filtered by `CONFIG_OPDI_NET_SCAN_MIN_RSSI` | Implemented |
| POST | /api/v1/net/ap/config | Override & persist AP SSID/channel (`{ssid?,channel?}`) | Implemented |

## WebSocket Events
Weak emitter stubs from `opdi_net` are overridden by the `opdi_api` component's WebSocket hub (`/ws`). On state transitions / IP acquisition JSON frames are broadcast to all connected clients:
```
{"type":"net","sub":"sta_connected","ip":"192.168.x.x","rssi":-52}
{"type":"net","sub":"sta_disconnected","reason":8}
{"type":"net","sub":"ap_active","ssid":"OPDI_SKPR-ABCD"}
```

## Logging & Observability
Tag: `opdi_net`. Logs state transitions & retry thresholds. Sensitive data (PSK) never logged; SSID fully omitted or minimally referenced.

## Kconfig Reference
```
CONFIG_OPDI_HOSTED_BUS="spi"
CONFIG_OPDI_COUNTRY_CODE="IL"
CONFIG_OPDI_NET_MAX_PROFILES=20
CONFIG_OPDI_NET_STA_TIMEOUT_S=60
CONFIG_OPDI_NET_RETRIES=20
CONFIG_OPDI_NET_RETRY_PERIOD_S=120
CONFIG_OPDI_NET_SCAN_MIN_RSSI=-90
CONFIG_OPDI_AP_CHANNEL=1
CONFIG_OPDI_AP_OPEN=y
CONFIG_OPDI_DHCP_HOSTNAME="opdi-skpr"
```

## Open TODOs (Next Phases)
1. Hidden SSID direct connect enhancements (channel/BSSID caching, active scan fallback).
2. LED tamper override logic & red LED priority integration.
3. Expanded unit tests (MRU rotation on timeout, retry->AP fallback path, scan filtering logic, WebSocket broadcast smoke test).
4. Security hardening: flash encryption, PSK obfuscation at rest, secure boot enable.
5. Auth/session layer & role gating for future config APIs.
6. Additional WebSocket event types (scan progress, retry counters, AP retry tick).
7. Camera / audio / system / OTA REST & WS API surfaces.
8. Structured metrics export (WS and/or MQTT integration) aligning with `OPDI_SKPR/system/metrics` topics.
9. Optional: embed minimal SPA assets (index.html, app.js, style.css) into flash for offline provisioning UI.

## Camera API (Phase 1 Stub)

The initial camera module (`opdi_cam`) provides configuration persistence and a static JPEG snapshot placeholder so the Web UI can integrate early without waiting for full sensor bring-up.

Component: `components/opdi_cam/`

### Config Data Model
Namespace: `cam` (NVS)
Key: `config` – binary blob of:
```
typedef struct {
	int brightness;   // -10..10 (clamped)
	int contrast;     // -10..10 (clamped)
	int saturation;   // -10..10 (clamped)
	bool auto_exposure; // true/false
} opdi_cam_config_t;
```
On first boot or decode failure defaults are applied (all 0, auto_exposure=true) and persisted. Values are clamped to [-10,10] for brightness / contrast / saturation.

### Public API
`esp_err_t opdi_cam_init(void);`
Loads persisted config (or defaults) into RAM. Future phases will also initialize the actual camera sensor.

`void opdi_cam_get_config(opdi_cam_config_t *out);`
Copies the in-memory config.

`esp_err_t opdi_cam_set_config(const opdi_cam_config_t *cfg);`
Validates/clamps fields, writes to NVS, updates in-memory struct.

`int opdi_cam_snapshot(unsigned char *buf, size_t buf_cap);`
If `buf==NULL` returns required buffer size (Phase 1 ~600 bytes). Otherwise writes a minimal valid JPEG (JFIF header + tiny pattern) and returns number of bytes written. The buffer always begins with SOI (0xFF 0xD8) and ends with EOI (0xFF 0xD9). Sensor capture integration will replace this in Phase 2 when a real frame is available; the fallback stub remains if sensor init fails.

### REST Endpoints
| Method | Path | Description | Notes |
|--------|------|-------------|-------|
| GET | /api/v1/cam/config | Returns current config `{brightness,contrast,saturation,auto_exposure}` | JSON | 
| POST | /api/v1/cam/config | Updates config (same JSON fields; all optional, unspecified fields keep previous values) | Clamped and persisted |
| GET | /api/v1/cam/snapshot | Returns `image/jpeg` payload | Currently static placeholder |

Error Handling:
* 400 if JSON malformed or field types invalid.
* 500 if persistence write fails.

### WebSocket (Future)
No camera-specific WS events yet. Planned events:
* `cam_snapshot` (async push or streaming keyframe)
* `cam_config_changed` (after POST)

### Phase 2 Roadmap
1. Integrate real sensor (init sequence & timing constraints) via esp_cam / esp_video pipeline.
2. Support dynamic resolution & format selection (JPEG quality, RGB565, YUV420).
3. Add frame streaming (chunked MJPEG over HTTP or WS binary frames).
4. Exposure/gain controls and auto-white-balance toggles.
5. Performance metrics (capture time, encode time, dropped frames).
6. Security: auth gating for config & snapshot endpoints.

### Testing
Implemented Unity tests:
* `test_opdi_cam_config_roundtrip` verifies default config, clamping (e.g. brightness=15 -> 10), persistence across NVS re-init, and auto_exposure toggle.
* `test_opdi_cam_snapshot` validates snapshot size contract (`size_query == write_size`), JPEG SOI/EOI markers, and sane upper bound (<4KB in stub mode).

Additional future tests will cover real sensor initialization fallback behavior and parameter application once hardware capture path is enabled.

---
Document section added Sep 2025 reflecting Phase 1 camera stub implementation.

## Testing Strategy (Planned)
* Host-based unit tests for MRU & retry logic (no hardware radio dependency).
* Target integration tests to validate AP fallback after 20 failures and periodic retry behavior.

---
This document will evolve as Phase 1 placeholders are replaced by fully functional implementations.
