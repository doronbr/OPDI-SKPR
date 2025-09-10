# OPDI_SKPR Web UI Specification (Phase 1-2)

## Goals
Provide a lightweight, responsive single-page interface for provisioning, monitoring, and basic control of the OPDI_SKPR device using only the on‑device HTTP server (no external CDN dependencies).

## Guiding Principles
- Offline-first: All assets served locally (SPIFFS or embedded) so fallback AP mode is fully functional without Internet.
- Low memory & latency: Minimal JS (< ~25 KB minified target initial), no heavy frameworks. Vanilla JS + small helper utilities.
- Progressive enhancement: Core provisioning (add Wi‑Fi, view status) must work even if WebSocket fails (poll REST fallback).
- Secure-by-default path: Session auth (Phase 2) wraps mutating endpoints; CSRF token stub in place early.
- Deterministic API contracts: All REST responses wrap `{ ok: true, data: {...} }` or `{ ok: false, error: "...", code: <int> }` (to be standardized).
- Accessibility (a11y) baseline: Focus order, ARIA roles, high contrast theme support from start.
- Internationalization readiness: `data-i18n="key.path"` attributes on textual nodes; translation map loaded dynamically (Phase 2).

## Feature Matrix
| Feature | Phase | Transport | Notes |
|---------|-------|-----------|-------|
| Device status (state/IP/RSSI) | 1 | REST + WS | `/api/v1/net/sta/status`, WS `net/sta_connected` / `sta_disconnected` / `ap_active` |
| Scan for networks | 1 | REST | GET `/api/v1/net/scan` active scan, button gated by debounce timer |
| Add Wi‑Fi profile | 1 | REST | POST `/api/v1/net/sta/profiles` (fields: ssid, psk, hidden?) |
| List / forget profiles | 1 | REST | GET `/api/v1/net/sta/profiles`; DELETE hash tail |
| Force connect | 1 | REST | POST `/api/v1/net/sta/connect` optional ssid |
| AP config override | 1 | REST | POST `/api/v1/net/ap/config` |
| Live event stream | 1 | WS | `/ws` JSON broadcast to update UI reactively |
| Auth/session | 2 | REST | Login endpoint + session cookie + logout |
| OTA upload | 2 | REST | Multi-part or chunked endpoint (TBD) |
| Factory reset | 2 | REST | POST confirmation token pattern |
| Camera preview (placeholder) | 2 | MJPEG/REST | `/stream` stub image cycling |
| Metrics panel | 2 | REST/WS | Basic counters (connect_attempts, success_count, scan_count) |
| Audio controls stub | 2 | REST/WS | Placeholder endpoints return not-implemented |

## UI Layout
```
+---------------------------------------------------------+
|  Header: Device Name | State Badge | IP | Uptime         |
+------------------+-------------------------------+-------+
| Left Nav         | Main Content Panel                    |
| - Status         | 1. Status Card (State, RSSI, IP)      |
| - Wi-Fi Profiles | 2. Scan & Add Network Form            |
| - Network Scan   | 3. Saved Profiles Table               |
| - System         | 4. AP Config (SSID/Channel)           |
| - Settings       | 5. Metrics (Phase 2)                  |
| - About          | 6. Logs (Phase 2)                     |
+---------------------------------------------------------+
| Footer: Build info | Firmware Version | Links            |
+---------------------------------------------------------+
```

## Component Responsibilities
- AppController: Boot sequence, WS connection management, global event dispatch.
- StatusPanel: Renders state, IP, RSSI; handles transitions coloring.
- ScanManager: Issues scan, handles cooldown, merges results list.
- ProfilesTable: Renders profiles JSON; forget action; triggers connect.
- AddProfileForm: Validation (SSID required, PSK length), dispatch POST.
- APConfigForm: Shows/persists fallback AP SSID/channel.
- ToastManager: Uniform success/error messages.
- MetricsPanel (Phase 2): Periodic pull or WS aggregated metrics.
- AuthDialog (Phase 2): Login / Logout flows.

## State Model (Front-end)
```js
state = {
  net: { currentState: 'INIT|STA_CONNECT|STA_CONNECTED|AP_ACTIVE', ip: '', rssi: null, ap: { ssid: '', channel: 1 } },
  profiles: [], // [{id, ssid_len, hidden, success}]
  scan: { results: [], lastRun: 0, running: false },
  metrics: { connect_attempts: 0, success_count: 0, scan_count: 0 },
  session: { authenticated: false, user: null, csrf: null },
  ui: { toasts: [] }
};
```

## WebSocket Protocol
Incoming events already defined:
```
{ "type":"net", "sub":"sta_connected", "ip":"<ip>", "rssi": -52 }
{ "type":"net", "sub":"sta_disconnected", "reason": 8 }
{ "type":"net", "sub":"ap_active", "ssid":"OPDI_SKPR-ABCD", "channel":1 }
```
Planned additions (Phase 2):
```
{ "type":"net", "sub":"scan_update", "count": N }
{ "type":"metrics", "connect_attempts": n, "scan_count": m }
{ "type":"ota", "sub":"progress", "pct": 37 }
```
Client should silently ignore unknown `type` / `sub` values.

## REST Response Normalization (Planned)
Wrapper structure:
```
// Success
{ "ok": true, "data": { ...payload... } }
// Error
{ "ok": false, "error": "human readable", "code": 123 }
```
Phase 1 endpoints currently return raw JSON or arrays; upgrade will be non‑breaking by still allowing legacy parse with feature detection.

## Error & Retry Strategy
- WS reconnect with exponential backoff: 1s,2s,4s,8s (cap 15s) with jitter.
- REST calls time out client-side at 8s; show toast on network error.
- Scan button disabled for `scan_debounce_s` (30s) after a successful scan.

## Theming & Accessibility
- CSS variables for color palette: `--color-bg`, `--color-accent`, `--color-badge-ok`, `--color-badge-warn`.
- High contrast toggle stores preference in `localStorage`.
- ARIA roles: navigation landmarks, live region for toasts (`aria-live="polite"`).

## Minimal Asset Footprint Targets
- index.html: < 3 KB
- app.js (unminified dev): < 30 KB
- style.css: < 8 KB

## Build / Deployment
Phase 1: Raw files served from `/web/*` path (SPIFFS).  
Phase 2: Optional esbuild step generating `dist/app.min.js` & content hashes; embed into binary via `idf_component_register(EMBED_FILES ...)`.

## Static File Serving Strategy
Option A (initial): Direct SPIFSS path mapping: when request path starts with `/ui` or `/` root returns `index.html`.  
Option B (Phase 2): Embed assets to reduce mount latency; maintain fallback to SPIFFS for development.

## Security Roadmap
1. Session cookie (HttpOnly, SameSite=Strict) + login endpoint.
2. CSRF token injected via Set-Cookie or meta tag after auth.
3. Transition provisioning AP endpoints to require auth except initial profile creation if no profiles present.
4. TLS (HTTPS) once certificate provisioning path available.

## Open Questions
- Should scan results cache time be user-tunable? (Default: rely on debounce only.)
- Do we expose raw RSSI for each saved profile (requires additional storage / last seen field)?
- Where to surface firmware update channel (stable/beta) selection? (system settings page?)

## Implementation Order (Proposed Next Commits)
1. Scaffold assets (`web/index.html`, `web/style.css`, `web/app.js`) + placeholder UI elements.
2. Static file handler in `opdi_api` component (serve `/` -> index, `/app.js`, `/style.css`).
3. Introduce response normalization helper (optional incremental).
4. Add basic ToastManager + WS connect logic.
5. Add forms + scan flow.
6. Session/auth skeleton if prioritized.
7. Metrics & extended events.

---
Document version: 0.1.0  
Last updated: (auto-update pending integration)
