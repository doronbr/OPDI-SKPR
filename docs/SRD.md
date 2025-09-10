# OPDI_SKPR â€“ Software Requirements Document (SRD)

# OPDI\_SKPR Face Recognition – Software Requirements Document (SRD)

**Project**: Face Detection & Recognition on Waveshare ESP32‑P4 Function EV Board (no LCD)
**Camera**: OV5647 (per provided schematics/datasheets)
**SDK/Env**: ESP‑IDF v5.5, VS Code
**Networking**: Wi‑Fi 6 on-board; web UI for configuration + video streaming
**Reference**: ESP‑WHO/ESP‑EYE style ensembles
**Device Name (hostname/mDNS/MQTT root)**: OPDI\_SKPR
**Document Owner**: doron brosh
**Status**: Draft v0.1
**Last Updated**: 2025-08-26

---

## 1. Purpose & Scope

* **Purpose**: Specify software requirements to implement on‑device face detection/recognition with web UI, two‑way voice, and TTS on the ESP32‑P4 board.

* **In Scope**: camera capture, inference pipeline, recognition DB, web streaming/config, **two‑way voice (web ↔ device)**, **on‑device TTS announcements**, Wi‑Fi setup, logging, OTA, CI.

* **Out of Scope**: LCD UI, battery power design, cloud ML training (unless specified), non-face object detection.

## 2. Stakeholders & Users

* **Primary users**: System integrators / developers configuring the device via Web UI.
* **Secondary**: Operators viewing live stream and recognition events.
* **Support/maintenance**: Firmware engineers, QA, DevOps.

## 3. System Overview

* **HW**: ESP32‑P4 Function EV Board, OV5647 camera (MIPI‑CSI sub‑board), on‑board USB‑C (UART/JTAG), MicroSD, Ethernet (RMII PHY), Wi‑Fi6 module, audio codec + amp (mic + speaker). No LCD.
* **SW**: ESP‑IDF components, ESP‑WHO derived modules, web server, recognition DB.
* **High‑level data flow**: Camera → ISP/CSI → Preproc → Detector → Recognizer → DB match → Events → Web UI/REST + MQTT + Audio/TTS.

### 3A. Hardware Overview (Board v1.5.2)

**Compute & Memory**

* ESP32‑P4 SoC (RISC‑V) with external QSPI flash; boot/reset/strap pins and 20×2 GPIO header ("CNN\_GPIOxx").
* MicroSD: SDMMC (4‑bit) and secondary SD bus available.

**Video**

* Camera via **MIPI‑CSI** connector (J9/J1), 2 data lanes + clock; camera control over I2C (`ESP_I2C_SCL/SDA`).
* Display connector **MIPI‑DSI** present (unused for this project).

**Audio**

* **ES8311** audio codec over **I2S4** (`MCLK/SCLK/LRCK/DSDIN/ASDOUT`) + I2C control; analog **electret mic** input and **NS4150B** mono power amplifier driving speaker outputs (SPK±).
* **PA\_CTRL** (amp enable/mute) available.

**Networking & I/O**

* **Wi‑Fi 6 module** (ESP32‑C6‑MINI‑1) with enable/wakeup lines.
* **Ethernet RMII** PHY with standard RMII signals.
* **USB‑C** ports: device/debug (USB‑UART/JTAG) and USB FS; Type‑C power‑path + protection.
* **On‑board keys/LEDs**: BOOT/RESET, user keys, status LEDs.

**Power**

* 5V input via USB‑C; on‑board buck/LDO rails for 3V3/AVDD; battery pad for RTC (if populated).

**Connectors of interest**

* **CSI**: primary camera connector (mates to camera sub‑board).
* **20×2 GPIO header**: exposes many `CNN_GPIOxx` signals; proposed default **TTS trigger input** routed here (see §F).

### 3B. Hardware‑to‑Software Mapping (Initial)

| Subsystem     | Signals (logical)                                                                                                       | Driver/Stack                  | Notes                                                      |
| ------------- | ----------------------------------------------------------------------------------------------------------------------- | ----------------------------- | ---------------------------------------------------------- |
| Camera        | MIPI‑CSI (2‑lane), I2C control                                                                                          | esp32‑p4 CSI + sensor driver  | OV5647 @ 1280×720/640×480/320×240 profiles                 |
| Audio Codec   | I2S4 + I2C                                                                                                              | ESP‑ADF optional / custom I2S | ES8311; 16 kHz mono for WS duplex                          |
| Audio Amp     | `PA_CTRL` (GPIO)                                                                                                        | GPIO                          | Enable/mute control                                        |
| Mic           | analog to ES8311                                                                                                        | Codec path                    | Electret mic (mono)                                        |
| Speaker       | SPK± via NS4150B                                                                                                        | Codec → Amp                   | Volume via codec/amp                                       |
| Network       | Wi‑Fi6 (C6)                                                                                                             | esp‑wifi (C6), lwIP           | STA/AP; WPA2/WPA3                                          |
| Ethernet      | RMII PHY                                                                                                                | esp‑eth                       | Optional                                                   |
| Storage       | QSPI flash, MicroSD                                                                                                     | SPI/SDMMC                     | Gallery/TTS assets on flash FATFS                          |
| Debug         | USB‑UART/JTAG                                                                                                           | idf.py monitor                | Flash, logs                                                |
| GPIO Triggers | `CNN_GPIO46` (RF TTS, **active‑high**) and `CNN_GPIO27` (Tamper, default **active‑low**)                                | GPIO ISR                      | RF receiver on GPIO46; tamper on GPIO27; both configurable |
| LEDs/IR       | `CNN_GPIO22` (**GREEN LED, active‑low**), `CNN_GPIO21` (**RED LED, active‑low**), `CNN_GPIO20` (**IR LED, active‑low**) | GPIO                          | IR LED via transistor recommended                          |

### 3C. Pin Map (Proposed)

* **Inputs**

  * `GPIO46` (header): **RF TTS trigger**, **active‑high** (sub‑GHz receiver output).
  * `GPIO27` (header): **Tamper input**, default **active‑low** with internal pull‑up; configurable polarity + debounce.
* **Outputs**

  * `GPIO22`: **GREEN LED**, **active‑low** (status OK/ready).
  * `GPIO21`: **RED LED**, **active‑low** (fault/warning/recording).
  * `GPIO20`: **IR LED enable**, **active‑low** (use low‑side transistor driver; max 10 mA direct drive not allowed).
* **Notes**: All pins configurable in UI/NVS; boot straps avoided; subject to board rev. v1.52 verification.

## 4. Device Functionality (Functional Requirements)

FR‑1. Boot & provision over Wi‑Fi AP mode; switch to STA after provisioning. Provisioning AP SSID: `OPDI_SKPR-XXXX` (last 4 hex of MAC). Default hostname/mDNS: `opdi-skpr.local`. FR‑2. Camera capture at configurable resolution/FPS: **1280×720 @15 FPS** and **640×480 @15 FPS** (default 640×480). Also support 320×240 for low‑latency mode. FR‑3. Face detection (real‑time) and optional tracking/smoothing across frames. FR‑4. Face embedding/recognition against on‑device gallery; enrollment supported from **live stream** and **file upload via Web UI**. FR‑5. Face gallery CRUD: **add, rename, delete, list**; per‑entry label + embedding. FR‑6. Web UI: live video stream, status, configuration, gallery management. FR‑7. REST/WS APIs for configuration, streaming, and events; downloadable gallery JSON. FR‑8. Logging and diagnostics via Web UI & serial. FR‑9. OTA firmware update via Web UI. FR‑10. Event export (JSON) over WebSocket/HTTP to client. FR‑11. **MQTT** publish/subscribe: recognition/detection events out; optional remote config in (scoped). FR‑12. Time sync via SNTP; timestamps in ISO‑8601. FR‑13. Config persistence in NVS (with schema versioning). FR‑14. Factory reset (**Web UI only**) and safe‑mode boot. FR‑15. Multi‑profile presets (Performance/Balanced/Quality). FR‑16. **Two‑way voice** between Web UI and device: **full‑duplex preferred (default)** with PTT fallback; browser mic → device speaker; device mic → browser playback. FR‑17. **On‑device TTS**: voice announcements for events (e.g., recognized person), and on‑demand TTS via API/UI. FR‑18. **GPIO triggers**: (a) **RF TTS trigger** on **GPIO46, active‑high** (sub‑GHz RX); (b) **Tamper** input on **GPIO27** (default **active‑low**, configurable). Both trigger TTS announcement (e.g., last recognized label) with debounce/hold‑time settings. FR‑19. **IR Night Vision control**: **GPIO20 active‑low** output to enable IR LED array; automatic night mode via luminance threshold (optional) and manual override in Web UI. FR‑20. **Status LEDs**: **GPIO22 GREEN (active‑low)** and **GPIO21 RED (active‑low)** with defined patterns (see Appendix G).

### 5.1 Performance Requirements

* **Detection/Recognition mode (low‑latency)**: **≥10 FPS @ 320×240**, end‑to‑end event latency **≤ 500 ms** (camera capture → embedding → match → event emitted).
* **Streaming (video)**: MJPEG at 640×480 or 1280×720 should sustain **15 FPS** when detection is **enabled**; if H.264 is enabled, target equal or higher FPS with ≤10% additional CPU.
* **Audio (two‑way)**: End‑to‑end mouth‑to‑ear **≤ 500 ms** on LAN for PTT; full‑duplex target **≤ 350 ms** where feasible. Glitch rate < 1% packet loss concealed.
* **TTS**: First utterance latency **≤ 1.5 s** from API call; subsequent utterances **≤ 800 ms** (cache), sample rate 16 kHz (or 22.05 kHz if budget allows).
* **Memory**: Budget to avoid heap exhaustion/frag: camera buffers + models + networking + **audio/TTS buffers/codecs** concurrently. Detailed budget in §7; PSRAM usage **if present**; otherwise fit in internal SRAM by profile.
* **Boot time**: Cold boot to ready (AP mode or last STA) **≤ 8 s**.
* **Storage**: Gallery capacity **≥ 32 identities** (configurable); JSON export/import practical within storage limits; **TTS voice assets/cache ≤ 4 MB** shared with `storage`.

### 5.2 Reliability & Availability

* **Uptime** target, watchdog, brownout, crash recovery, core dumps, health checks.

### 5.3 Security Requirements

* **Wi‑Fi**: WPA2/WPA3‑Personal in STA mode.
* **Web UI/API Auth**: Username/password (stored hashed); session cookie.
* **Transport**: **Phase 1**: HTTP (local/LAN) with CSRF/XSS protections. **Phase 2**: add TLS (HTTPS) + certificate management; optional mTLS for APIs.
* **Audio security**: Disable browser mic access by default; require explicit user action (PTT) and authenticated session. Rate‑limit TTS and inbound audio APIs; optional ACLs for MQTT‑triggered TTS.
* **Firmware**: OTA signing; plan for Secure Boot + Flash Encryption (enable behind build flag initially).
* **Secrets**: Credentials in NVS with key‑scoping and erase‑on‑factory‑reset.

### 5.4 Maintainability & Observability

* Logging levels, metrics (FPS, latency, memory), structured logs, crash dumps; feature flags.
* Config schema migration & rollback.

## 6. Interfaces

### 6.1 Web UI (no LCD)

* Pages: Dashboard (preview + stats), Settings (network, camera, pipeline), **Audio (voice & TTS)**, Gallery, Logs/Diagnostics, OTA.
* Live stream: **MJPEG (baseline)** at 640×480/1280×720; **Investigate H.264** if feasible on ESP32‑P4 (codec or software) and gate by CPU/memory.
* Gallery: add (capture/upload), rename, delete, export/download JSON; import JSON (optional v2).
* Audio tools: Full‑duplex (default) with toggle to PTT, speaker volume slider, mic input selector/level meter, TTS text box with "Speak" button; playback widget.
* Auth: Username/password with session cookie.

### 6.2 APIs

* **REST**: `/api/v1/*` (config, gallery, system, audio, tts). Endpoints include:

  * `GET /api/v1/system/info`
  * `GET /api/v1/camera/frame`
  * `GET /api/v1/gallery` / `POST /api/v1/gallery` (add) / `PUT /api/v1/gallery/{id}` (rename) / `DELETE /api/v1/gallery/{id}`
  * `GET /api/v1/gallery/export` (JSON download) / `POST /api/v1/gallery/import` (optional v2)
  * `GET /api/v1/config` / `PUT /api/v1/config`
  * **Audio**:

    * `WS /audio/ws` (**binary PCM16** frames; 20 ms; header `{seq:uint16, ts_ms:uint32}`)
    * `GET /api/v1/audio/config` / `PUT /api/v1/audio/config` (codec, sample rate, PTT vs full‑duplex)
  * **TTS**:

    * `POST /api/v1/tts` `{text, voice, speed, volume, priority}` → speak now (**202 Accepted**)
    * `GET /api/v1/tts/voices` → available voices (JSON)
* **WebSocket**: `/ws` for events `{type:"detect"|"recognize", bbox, id, score, ts}`.
* **Streaming**: `/stream` (MJPEG), `/h264` (if supported).
* **OpenAPI**: auto‑generated spec (see Appendix I2).

> **Implemented in seed repo:** `/`, `/stream`, `/ws`, `/api/v1/system/info`, `, `, \`\`.

### 6.3 MQTT

* **Topics (outbound)**: `OPDI_SKPR/face/events` (per detection/recognition JSON), `OPDI_SKPR/system/metrics` (FPS/mem), `OPDI_SKPR/tts/played`.
* **Inbound commands**: none in v1 (physical IO is **TTS‑only**).
* **QoS**: 0 or 1 (configurable). Retain off for events.

### 6.4 Audio & TTS (new)

* **Hardware**: Mic + speaker + ES8311 codec over **I2S + I²C** per schematics.
* **Codecs (initial)**: Browser ↔ device via **PCM16 @ 16 kHz** over WS for simplicity; **Opus** evaluation for v2 to reduce bandwidth/CPU.
* **Duplex & AEC/NS**: Full‑duplex default; PTT available; basic noise suppression; echo cancellation best‑effort (depends on audio HW topology).
* **Framing (WS)**: 16 kHz mono **PCM16**, **20 ms frames (320 samples)** per binary WS message with a 6‑byte header `{seq:uint16, ts_ms:uint32}` followed by PCM payload; target jitter buffer **80 ms** (60–120 ms adaptive).
* **TTS engine**: Lightweight on‑device engine; cache last N utterances; configurable voice, speed, volume. **Default voice: English female. Auto‑announcements enabled by default** (e.g., “Hello, {label}”) with debounce window.
* **Triggers**: Auto‑announce recognized labels (debounced). **Physical IO is TTS‑only** (hardware line can trigger an announcement).

#### 6.4.1 Implementation status (audio/TTS v0.3)

* **Codec bring‑up**: **ES8311** initialized via **esp\_codec\_dev** (I²C control + I2S data). Defaults: **16 kHz/16‑bit/mono**, output volume **60%**, mic input gain **≈ 26 dB**, **PA\_EN** asserted when active.
* **I2S**: std‑mode master; pins configurable via Kconfig (`OPDI_AUDIO_PIN_{MCLK,BCLK,WS,DOUT,DIN,PA_EN}`); controller `OPDI_AUDIO_I2S_PORT`.
* **Shared speaker path**: `/audio/ws` downlink and TTS both enqueue into the **same speaker ring buffer** → single drain to I2S/DAC.
* **PicoTTS wired**: **PicoTTS** produces 16 kHz PCM via callbacks; queue length = 4; idle/error callbacks signal completion.
* **Runtime controls**: `opdi_audio_set_volume_percent(0..100)`; TTS REST `POST /api/v1/tts` enqueues speech; `GET /api/v1/tts/voices` lists voices.

## 7. Data & Memory Mapping

Data & Memory Mapping

* **Partitions (32 MB flash target)**: `factory`, `ota_0`, `ota_1`, `nvs`, `phy_init`, `storage` (gallery **+ TTS cache/assets**), `coredump`. See Appendix A.
* **Gallery**: fixed‑length records: `{id, label, embedding}`; embeddings + labels persisted in `storage`; **capacity: ≥ 32 identities** (configurable). Export format: JSON array with base64/int8 embedding representation.
* **Audio buffers**: duplex ring buffers for mic and speaker paths; WS packet queues; jitter buffer 60–120 ms.
* **TTS assets**: voice tables/models cached under `storage/tts/`; budget ≤ 4 MB.
* **NVS keys**: namespaces `net`, `camera`, `pipeline`, `gallery_meta`, `ui`, `audio`, `tts`.
* **Heap/PSRAM strategy**: camera DMA buffers (framequeue), detector/recognizer workspaces, **audio/TTS buffers and codecs**, network stacks; prefer static pools and zero‑copy where possible.
* **RTC memory**: fast boot flags.

## 8. Project Structure & Selection

* Monorepo layout under ESP‑IDF: `components/`, `main/`, `tools/`.
* Build profiles (Release/Debug), feature flags via Kconfig.
* Selectable build targets via CMake options: `who_web` (full), `detector_only`, `recognizer_only`, `sim_host`.
* Board support: pin‑map for OV5647 (CSI/I2C) and camera power rails per schematics.

## 9. External Repositories & Licenses

#### Pinned versions (Audio/TTS)

* **esp\_codec\_dev**: `1.4.0`
* **es8311**: `1.0.0~1`
* **picotts**: `1.1.3`

**Baseline (Required)**

* **ESP‑IDF v5.5** – Core SDK (FreeRTOS, drivers, networking, NVS, FATFS, mbedTLS, lwIP, SNTP, mDNS, OTA, HTTP server, etc.).
* **Camera Controller (ESP32‑P4)** – Built‑in IDF driver for **MIPI‑CSI/ISP** on P4; primary interface for OV5647 via `esp_cam_sensor` (see below).
* **JPEG Encoder/Decoder (IDF)** – For MJPEG streaming and snapshot encoding.
* **ESP‑HTTP‑Server (IDF)** with **WebSocket (WS) support** – REST + WS APIs and streaming endpoints.
* **ESP‑MQTT (IDF)** – MQTT client for events/metrics.
* **NVS & FATFS** – Persist settings + gallery/TTS assets.

**Vision stack**

* **ESP‑WHO** – Face detection/recognition reference apps/components (P4 supported). Used as the starting point for detector/recognizer and gallery flow.
* **ESP‑DL** – Inference kernels and model runtime used by ESP‑WHO.
* **esp‑video‑components / esp\_cam\_sensor (OV5647)** – Sensor driver package and camera utilities for CSI/ISP path on P4 (OV5647 profile).

**Video streaming / codecs**

* **MJPEG (baseline)** – IDF JPEG enc/dec + `/stream` endpoint.
* **H.264 (investigate)** – **ESP‑H264** component targeting **ESP32‑P4 HW encoder**; evaluate RTSP/WS transport and CPU/mem impact before enabling.

**Audio & voice**

* **ESP‑ADF** (Audio Dev Framework) or \`\`\*\* minimal stack\*\* – I2S pipeline, device drivers; **ES8311** codec support.
* **On‑device TTS (English)** – **PicoTTS component** (`picotts`) with English voice data; 16‑kHz PCM output.
* **(Alt./Optional)** **ESP‑TTS** (primarily CN voices) for future multilingual support; **Opus encoder** component for future low‑bandwidth duplex.

**Utilities & tooling**

* **cJSON** (or parity JSON lib) – REST payloads and export/import.
* **Unity (IDF)** – Unit test framework (host/target).

**Licenses**

* Prefer Apache‑2.0 / BSD‑3‑Clause (Espressif OSS). Third‑party components (e.g., PicoTTS, Opus) must include NOTICE and license texts in `third_party/` and be tracked in `LICENSES.md`.

## 10. Coding Conventions Coding Conventions

* C/C++: MISRA‑inspired where practical, clang‑format, clang‑tidy, doxygen comments.
* ESP‑IDF style for error handling (`ESP_ERROR_CHECK`, `esp_err_t`), task naming, module prefixes.
* Logging: `ESP_LOG*` with domain tags, structured fields.
* Concurrency: FreeRTOS rules, ownership, zero‑copy where possible.
* Assertions, defensive programming, unit/integration testability.

## 11. Testing Strategy

* **Unit tests**: Unity (IDF) with host & target runners.
* **Integration**: camera → pipeline; network → API; OTA; storage; MQTT publish; **audio WS duplex**; **TTS playback**.
* **System/Soak**: long‑run with leak checks, heap tracing.
* **Simulators**: file‑backed camera frames, PC host inference stubs; offline replay of recorded streams; **synthetic audio generator and loopback**.
* **Performance tests**: automated FPS/latency/memory dashboards; acceptance for 10 FPS @ 320×240 and ≤500 ms latency; **audio RTT and dropout rate**; **TTS start/play latency**.
* **Test data**: curated face images/frames with labels; **audio clips** for NS/AEC evaluation.

## 12. Tooling, CI & Scripts

* GitHub Actions/Runner: build matrix (`idf.py set-target esp32p4`), unit tests, artifact uploads.
* Static analysis: clang‑tidy, cppcheck, `idf.py analyze-size`, heap fragmentation checks.
* Firmware packaging + OTA signing.
* Scripts: `tools/` for flashing, provisioning, log capture, perf probes; MQTT test publisher/subscriber.
* Versioning: SemVer + build metadata.

## 13. Risks & Mitigations

* Camera driver stability; ISP settings; memory pressure; thermal throttling; Wi‑Fi throughput; legal/privacy implications; model accuracy drift.
* **Audio‑specific**: echo/feedback without proper AEC; CPU spikes due to codec/TTS; bandwidth contention with video; browser compatibility.

## 14. Acceptance Criteria

* Meets perf/security targets; passes test suite; CI green; stable streaming+recognition; reproducible builds; documentation complete.
* **Audio**: Two‑way voice works on Chrome/Edge/Firefox (desktop) on LAN; PTT latency ≤ 500 ms; full‑duplex (if enabled) ≤ 350 ms mouth‑to‑ear; TTS plays within target latencies and is intelligible (MOS ≥ 3.5 subjective).

## 15. Compliance & Privacy

* Data retention policy; GDPR‑like consent notes if storing faces; opt‑out; anonymization options.

## 16. Glossary

* **Detector**: network that finds face bounding boxes.
* **Recognizer/Embedding**: network that converts a face crop to a vector.
* **Gallery**: local DB of embeddings with IDs/labels.
* **TTS (Text‑to‑Speech)**: on‑device synthesis of speech audio from text.

### Appendix A – Partition Table CSV (Draft)

```csv
# Name, Type, SubType, Offset, Size, Flags
nvs,data,nvs,0x9000,24K,
phy_init,data,phy,0xF000,4K,
factory,app,factory,0x10000,4M,
ota_0,app,ota_0,,4M,
ota_1,app,ota_1,,4M,
storage,data,fat,,16M,
coredump,data,coredump,,1M,
```

> Notes: Offsets for OTA/storage/coredump auto‑assigned by build system based on CSV order; sizes may adjust once image size is known.

### Appendix B – Memory Budget (Target)

| Category                          | Budget         | Notes                                |
| --------------------------------- | -------------- | ------------------------------------ |
| Camera frame buffers (MJPEG ring) | ≤ 0.8 MB       | 2–3 frames @ 640×480/720p compressed |
| Preproc + detector workspaces     | ≤ 2.0 MB       | grayscale/downscale + NN tmp buffers |
| Recognizer model + workspace      | ≤ 2.5 MB       | embedding net + scratch              |
| Networking stacks (HTTP/WS/MQTT)  | ≤ 0.4 MB       | sockets, TLS (phase 2) excluded      |
| Audio duplex + jitter buffers     | ≤ 0.25 MB      | 16 kHz PCM, 80–120 ms jitter         |
| TTS runtime scratch               | ≤ 0.5 MB       | synth working memory                 |
| Free headroom at runtime          | ≥ 20% of total | watchdog thresholding                |
| **Total peak target (PSRAM)**     | **≤ 6.5 MB**   | excluding flash‑resident assets      |

> If PSRAM is unavailable, fall back to 320×240 video + lower concurrency (disable TTS while streaming), preserving perf targets.

### Appendix C – Audio Pipeline Spec (v0.3) (updated)

**Additions in v0.3 (implemented):**

* **Codec init (ES8311 via esp\_codec\_dev)**: create I2S data IF + I²C control IF → `es8311_codec_new(…WORK_MODE_BOTH…)` → `esp_codec_dev_new()` → `esp_codec_dev_open(16k/mono/16b)` → set **speaker volume 60%** and **mic gain ≈ 26 dB** → assert **PA\_EN**.

* **Speaker path**: 64 KB ring buffer (\~2 s @ 16 kHz); drain task calls `esp_codec_dev_write()`.

* **TTS**: **PicoTTS** callback feeds PCM into the same speaker buffer; queue length 4; completion via idle callback.

* **I2S**: 16 kHz, mono, 16‑bit; device ↔ codec per schematics; internal task priority `CONFIG_OPDI_AUDIO_TASK_PRIO` (default 9).

* **Buffers**: mic ring = 8×20 ms; spk ring = 8×20 ms; jitter buffer target 80 ms.

* **Web UI**: getUserMedia 16 kHz mono (downsample if needed); AudioWorklet for packetization; auto‑gain off.

* **PTT**: UI button holds transmission; full‑duplex toggle persists in NVS.

* **TTS**: queue with max length 3; policy = drop oldest or interruptable; debounce window for auto‑announce default 5 s per identity.

### Appendix D – WebSocket Framing (Audio)

* **Endpoint**: `WS /audio/ws` (single socket). **Text frames** carry control messages; **binary frames** carry audio.
* **Control message (JSON)**:

```json
{"type":"hello","codec":"pcm16_16k_mono","role":"browser|device"}
```

* **Binary audio frame layout** (little‑endian):

```
0..1  : seq (uint16 wraps)  
2..5  : ts_ms (uint32, sender clock)  
6..end: PCM16 samples (320 samples for 20 ms)
```

* **Directions**: client→server = mic uplink; server→client = speaker downlink.
* **Stats**: periodic text `{type:"stats", rx_jitter_ms, tx_q_depth, pkt_loss}`.

### Appendix E – REST/OpenAPI Schemas (Excerpts)

* **POST /api/v1/tts**

```json
{
  "text": "Hello, Alice",
  "voice": "en_female_1",
  "speed": 1.0,
  "volume": 0.8,
  "priority": "normal"  // "high" interrupts queue
}
```

* **GET /api/v1/tts/voices** → `[{"id":"en_female_1","name":"English Female"}]`
* **GET /api/v1/audio/config** → `{ "codec":"pcm16_16k_mono", "full_duplex":true, "jitter_target_ms":80 }`
* **PUT /api/v1/audio/config** body mirrors keys above.
* **Events (WS /ws)**: recognition

```json
{"type":"recognize","id":"person_12","label":"Alice","score":0.92,"bbox":[120,88,80,80],"ts":"2025-08-26T12:00:00Z"}
```

### Appendix F – GPIO Triggers (RF TTS + Tamper)

* **Purpose**: Hardware events that trigger **TTS announcements** and **LED indications**.
* **Defaults** (board v1.52):

  * `IO_TRIG_TTS_RF` → **GPIO46**, **active‑high**, edge‑trigger; min pulse 50 ms.
  * `IO_TRIG_TAMPER` → **GPIO27**, **active‑low** with internal pull‑up; edge‑trigger; min pulse 50 ms.
* **Config**: exposed in Web UI & NVS (`io/*`): polarity, debounce (default 30 ms), action (speak last label/custom message), throttle window (default 5 s).
* **LED**: When **Tamper = OPEN**, force **RED LED ON** with **highest priority override** (see Appendix G). Clear override when tamper **CLOSED**.
* **Events**: every trigger publishes `OPDI_SKPR/io/event` with `{source:"RF_TTS"|"TAMPER", state, ts}`.

### Appendix G – LED Behavior (Default) (Default)

* **GREEN (GPIO22, active‑low)**

  * Solid ON: system ready (STA connected; services up)
  * Blinking 1 Hz: AP provisioning mode
  * Off: booting or fault (see RED)

* **RED (GPIO21, active‑low)**

  * Solid ON: **Tamper OPEN** (GPIO27 indicates open) — **highest‑priority override**
  * Solid ON (fallback): critical fault (watchdog pending, storage error, model load fail) — shown only if tamper is **closed**
  * Blinking 2 Hz: Wi‑Fi disconnected/reconnecting — **suppressed while tamper OPEN**
  * 200 ms flash on event: recognition (and tamper edge when CLOSED), rate‑limited — **suppressed while tamper OPEN**

**Priority (highest → lowest)**: **Tamper OPEN** → Critical Fault → Wi‑Fi Reconnecting → Event flash.

> All behaviors configurable under Settings → System → Indicators.

### Appendix H – Kconfig Options (Draft) (Draft)

| Key                                   | Default       | Notes                              |          |           |
| ------------------------------------- | ------------- | ---------------------------------- | -------- | --------- |
| `CONFIG_OPDI_HOSTNAME`                | `opdi-skpr`   | mDNS/hostname                      |          |           |
| `CONFIG_OPDI_DEVICE_NAME`             | `OPDI_SKPR`   | MQTT topic root                    |          |           |
| `CONFIG_OPDI_PROFILE`                 | `balanced`    | \`performance                      | balanced | quality\` |
| `CONFIG_OPDI_AUTH_ENABLED`            | `y`           | Username/password auth             |          |           |
| `CONFIG_OPDI_TLS_ENABLE`              | `n`           | Phase 2 → `y`                      |          |           |
| `CONFIG_OPDI_OTA_SIGNING`             | `y`           | Require signed OTA                 |          |           |
| `CONFIG_OPDI_FLASH_ENCRYPTION`        | `n`           | Phase 2 → `y`                      |          |           |
| `CONFIG_OPDI_MQTT_ENABLE`             | `y`           | Enable MQTT pub                    |          |           |
| `CONFIG_OPDI_MQTT_BROKER`             | \`\`          | e.g., `mqtt://192.168.1.10`        |          |           |
| `CONFIG_OPDI_TOPIC_PREFIX`            | `OPDI_SKPR`   | MQTT prefix                        |          |           |
| `CONFIG_OPDI_NET_MAX_PROFILES`        | `20`          | Max saved SSIDs                    |          |           |
| `CONFIG_OPDI_NET_STA_TIMEOUT_S`       | `60`          | Per SSID attempt                   |          |           |
| `CONFIG_OPDI_NET_RETRIES`             | `3`           | Before AP fallback                 |          |           |
| `CONFIG_OPDI_CAM_RES`                 | `640x480`     | \`1280x720                         | 640x480  | 320x240\` |
| `CONFIG_OPDI_CAM_FPS`                 | `15`          | Target FPS (best‑effort)           |          |           |
| `CONFIG_OPDI_STREAMING_MJPEG`         | `y`           | Baseline stream                    |          |           |
| `CONFIG_OPDI_STREAMING_H264`          | `n`           | Set `y` if feasible                |          |           |
| `CONFIG_OPDI_AUDIO_FULL_DUPLEX`       | `y`           | Full‑duplex default                |          |           |
| `CONFIG_OPDI_AUDIO_SR_HZ`             | `16000`       | Sample rate                        |          |           |
| `CONFIG_OPDI_AUDIO_JITTER_TARGET_MS`  | `80`          | 60–120 adaptive                    |          |           |
| `CONFIG_OPDI_AUDIO_TASK_PRIO`         | `9`           | I2S/WS task                        |          |           |
| `CONFIG_OPDI_TTS_DEFAULT_VOICE`       | `en_female_1` | English female                     |          |           |
| `CONFIG_OPDI_TTS_CACHE_SIZE_MB`       | `4`           | In `storage/tts/`                  |          |           |
| `CONFIG_OPDI_TTS_AUTO_ANNOUNCE`       | `y`           | Auto “Hello, {label}”              |          |           |
| `CONFIG_OPDI_TTS_ANNOUNCE_DEBOUNCE_S` | `5`           | Per identity                       |          |           |
| `CONFIG_OPDI_GPIO_TTS_RF`             | `46`          | Active‑high                        |          |           |
| `CONFIG_OPDI_GPIO_TAMPER`             | `27`          | Default active‑low                 |          |           |
| `CONFIG_OPDI_GPIO_LED_GREEN`          | `22`          | Active‑low                         |          |           |
| `CONFIG_OPDI_GPIO_LED_RED`            | `21`          | Active‑low                         |          |           |
| `CONFIG_OPDI_GPIO_IR_EN`              | `20`          | Active‑low                         |          |           |
| `CONFIG_OPDI_IDLE_POWERDOWN_S`        | `20`          | Camera/audio power‑down idle timer |          |           |
| `CONFIG_OPDI_FACE_TRIGGER_WINDOW_S`   | `5`           | Detection window on RF trigger     |          |           |
| `CONFIG_OPDI_PUSH_CHANNEL`            | `mqtt`        | v1 push via MQTT only              |          |           |

#### Audio/TTS‑specific Kconfig keys

| Key                           | Default | Notes                                     |
| ----------------------------- | ------- | ----------------------------------------- |
| `CONFIG_OPDI_AUDIO_HW_ENABLE` | `y`     | Enable I2S + ES8311 bring‑up              |
| `CONFIG_OPDI_AUDIO_I2S_PORT`  | `0`     | I2S controller index                      |
| `CONFIG_OPDI_AUDIO_PIN_MCLK`  | `-1`    | I2S MCLK GPIO (set per schematic)         |
| `CONFIG_OPDI_AUDIO_PIN_BCLK`  | `-1`    | I2S BCLK GPIO                             |
| `CONFIG_OPDI_AUDIO_PIN_WS`    | `-1`    | I2S WS/LRCK GPIO                          |
| `CONFIG_OPDI_AUDIO_PIN_DOUT`  | `-1`    | I2S DOUT (to codec)                       |
| `CONFIG_OPDI_AUDIO_PIN_DIN`   | `-1`    | I2S DIN (from codec)                      |
| `CONFIG_OPDI_AUDIO_PIN_PA_EN` | `-1`    | Power amp enable GPIO (high = unmute)     |
| `CONFIG_OPDI_TTS_USE_PICOTTS` | `y`     | Use PicoTTS engine (16 kHz PCM callbacks) |

### Appendix I2 – OpenAPI (Draft v0.1)

```yaml
openapi: 3.0.3
info:
  title: OPDI_SKPR Device API
  version: 0.1.0
servers:
  - url: http://{host}
    variables:
      host:
        default: opdi-skpr.local
security:
  - cookieAuth: []
components:
  securitySchemes:
    cookieAuth:
      type: apiKey
      in: cookie
      name: session_id
  schemas:
    Error:
      type: object
      properties:
        code: { type: string }
        message: { type: string }
    SystemInfo:
      type: object
      properties:
        device: { type: string, example: OPDI_SKPR }
        hostname: { type: string }
        fw_version: { type: string }
        uptime_s: { type: integer }
        wifi_rssi: { type: integer }
        profile: { type: string, enum: [performance, balanced, quality] }
    GalleryEntry:
      type: object
      properties:
        id: { type: string }
        label: { type: string }
        embedding: { type: array, items: { type: number }, description: int8/fp16 stored internally; export as base64 or numbers }
        created_at: { type: string, format: date-time }
    GalleryList:
      type: object
      properties:
        count: { type: integer }
        items:
          type: array
          items: { $ref: '#/components/schemas/GalleryEntry' }
    Config:
      type: object
      properties:
        net: { type: object, properties: { hostname: { type: string } } }
        camera: { type: object, properties: { res: { type: string }, fps: { type: integer } } }
        audio: { type: object, properties: { full_duplex: { type: boolean }, sr_hz: { type: integer }, jitter_target_ms: { type: integer } } }
        tts: { type: object, properties: { voice: { type: string }, auto_announce: { type: boolean }, debounce_s: { type: integer } } }
        io: { type: object, properties: { tts_rf_pin: { type: integer }, tamper_pin: { type: integer }, led_green: { type: integer }, led_red: { type: integer }, ir_en: { type: integer } } }
    AudioConfig:
      type: object
      properties:
        codec: { type: string, enum: [pcm16_16k_mono] }
        full_duplex: { type: boolean }
        sr_hz: { type: integer, enum: [16000, 22050] }
        jitter_target_ms: { type: integer, minimum: 20, maximum: 240 }
    TtsRequest:
      type: object
      required: [text]
      properties:
        text: { type: string }
        voice: { type: string, default: en_female_1 }
        speed: { type: number, default: 1.0 }
        volume: { type: number, default: 0.8 }
        priority: { type: string, enum: [low, normal, high], default: normal }
    Voice:
      type: object
      properties:
        id: { type: string }
        name: { type: string }
        lang: { type: string, example: en-US }
    RecognizeEvent:
      type: object
      properties:
        type: { type: string, enum: [recognize] }
        id: { type: string }
        label: { type: string }
        score: { type: number }
        bbox:
          type: array
          minItems: 4
          maxItems: 4
          items: { type: integer }
        ts: { type: string, format: date-time }
paths:
  /api/v1/system/info:
    get:
      summary: Get device info
      responses:
        '200':
          description: OK
          content:
            application/json:
              schema: { $ref: '#/components/schemas/SystemInfo' }
  /api/v1/gallery:
    get:
      summary: List gallery entries
      responses:
        '200':
          description: OK
          content:
            application/json:
              schema: { $ref: '#/components/schemas/GalleryList' }
    post:
      summary: Add entry by capture or upload
      requestBody:
        required: true
        content:
          application/json:
            schema:
              type: object
              properties:
                capture: { type: boolean, description: Capture from live camera }
                label: { type: string }
          multipart/form-data:
            schema:
              type: object
              properties:
                label: { type: string }
                file:
                  type: string
                  format: binary
      responses:
        '202': { description: Accepted }
        '400': { description: Bad request, content: { application/json: { schema: { $ref: '#/components/schemas/Error' } } } }
  /api/v1/gallery/{id}:
    parameters:
      - in: path
        name: id
        required: true
        schema: { type: string }
    put:
      summary: Rename entry
      requestBody:
        required: true
        content:
          application/json:
            schema:
              type: object
              properties: { label: { type: string } }
      responses: { '204': { description: Renamed }, '404': { description: Not found } }
    delete:
      summary: Delete entry
      responses: { '204': { description: Deleted }, '404': { description: Not found } }
  /api/v1/gallery/export:
    get:
      summary: Export gallery JSON
      responses:
        '200':
          description: OK
          content:
            application/json:
              schema: { $ref: '#/components/schemas/GalleryList' }
  /api/v1/config:
    get:
      summary: Get config
      responses:
        '200': { description: OK, content: { application/json: { schema: { $ref: '#/components/schemas/Config' } } } }
    put:
      summary: Update config
      requestBody:
        required: true
        content:
          application/json:
            schema: { $ref: '#/components/schemas/Config' }
      responses:
        '204': { description: Updated }
  /api/v1/audio/config:
    get:
      summary: Get audio config
      responses:
        '200': { description: OK, content: { application/json: { schema: { $ref: '#/components/schemas/AudioConfig' } } } }
    put:
      summary: Update audio config
      requestBody:
        required: true
        content:
          application/json:
            schema: { $ref: '#/components/schemas/AudioConfig' }
      responses:
        '204': { description: Updated }
  /api/v1/tts:
    post:
      summary: Speak text now
      requestBody:
        required: true
        content:
          application/json:
            schema: { $ref: '#/components/schemas/TtsRequest' }
      responses:
        '202': { description: Queued }
  /api/v1/tts/voices:
    get:
      summary: List voices
      responses:
        '200':
          description: OK
          content:
            application/json:
              schema:
                type: array
                items: { $ref: '#/components/schemas/Voice' }
```

> Note: WebSocket endpoints (`/ws`, `/audio/ws`) and MJPEG/H.264 streaming paths are documented in SRD but are outside OpenAPI scope.

### Appendix J – CI Workflow Outline (GitHub Actions)

```yaml
name: build
on: [push, pull_request]
jobs:
  esp32p4:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: espressif/esp-idf-ci-action@v1
        with:
          esp_idf_version: v5.5
      - name: Set target
        run: idf.py set-target esp32p4
      - name: Build
        run: idf.py build
      - name: Unit tests (host)
        run: pytest tests/host || true
      - name: Size report
        run: idf.py size-components
      - name: Artifacts
        uses: actions/upload-artifact@v4
        with:
          name: firmware
          path: build/*.bin
```

### Appendix K – Web UI ↔ Config Mapping

* **Network**: SSID/Pass → NVS `net/*`; hostname → `opdi-skpr` default.
* **Camera**: Resolution/FPS/profile → `camera/*`, `pipeline/*`.
* **Audio**: full‑duplex/PTT, volume, jitter target → `audio/*`.
* **TTS**: default voice, auto‑announce, debounce → `tts/*`.
* **IO**: RF TTS pin/polarity, Tamper pin/polarity, LED active‑low flags, IR enable → `io/*`.
* **System**: OTA, indicators, logging level → `system/*`.

### Appendix Z – Scripts & Dev Tools Plan

**Directory**: `tools/`

| File                     | Purpose                                                         | Usage                                                         |
| ------------------------ | --------------------------------------------------------------- | ------------------------------------------------------------- |
| `flash.sh` / `flash.ps1` | Wrap `idf.py -p <port> flash monitor` with target/env detection | `./tools/flash.sh -p COM5`                                    |
| `provision.py`           | CLI to set Wi‑Fi SSID/Pass, admin user, MQTT broker via REST    | `python tools/provision.py --host opdi-skpr.local --ssid ...` |
| `perf_probe.py`          | Measure FPS/latency via `/ws` metrics; export CSV               | `python tools/perf_probe.py --host ...`                       |
| `mjpeg_dump.py`          | Save snapshot stream frames for offline analysis                | `python tools/mjpeg_dump.py --url http://host/stream`         |
| `audio_loopback.html`    | Browser page to test `/audio/ws` duplex/PTT                     | Open in Chrome, select mic, start                             |
| `tts_cli.py`             | Send `POST /api/v1/tts` with text and options                   | `python tools/tts_cli.py --host ... "Hello"`                  |
| `gpio_pulse_gen.md`      | Breadboard guide to create 50 ms pulses for GPIO46              | —                                                             |
| `ota_sign.py`            | Sign firmware and prepare upload payloads                       | `python tools/ota_sign.py build/firmware.bin`                 |
| `metrics_tail.py`        | Subscribe to WS/MQTT metrics and print formatted output         | `python tools/metrics_tail.py --mqtt mqtt://...`              |

**Notes**: Python scripts depend on `requests`, `websockets`, `paho-mqtt` where applicable; include `requirements.txt`.

### Appendix AA – Repository Scaffold

```
opdi_skpr/
├─ CMakeLists.txt                 # see Appendix AJ (drop‑in)
├─ idf_component.yml              # pinned deps (Appendix AF)
├─ sdkconfig.defaults             # Kconfig defaults (Appendix H)
├─ partitions.csv                 # partition table (Appendix A)
├─ external/
│  └─ esp-who/                    # Git submodule (ESP‑WHO repo)
├─ components/
│  ├─ opdi_cam/
│  ├─ opdi_detect/
│  ├─ opdi_recog/
│  ├─ opdi_gallery/
│  ├─ opdi_stream/
│  ├─ opdi_api/
│  ├─ opdi_audio/
│  ├─ opdi_tts/
│  ├─ opdi_io/
│  ├─ opdi_mqtt/
│  ├─ opdi_metrics/
│  ├─ opdi_config/
│  └─ opdi_utils/
├─ main/
│  ├─ app_main.c
│  ├─ web_ui/                     # static assets → SPIFFS/FAT
│  └─ routes.c                    # REST endpoints
├─ tools/                         # per Appendix Z
├─ tests/
│  ├─ host/                       # pytest, mocks
│  └─ target/                     # Unity tests
├─ README-dev.md                  # Quickstart (Appendix AK)
└─ (generated) dependencies.lock  # after first resolve
```

### Appendix AB – Build & Run Commands

* **Set target & build**: `idf.py set-target esp32p4 && idf.py build`
* **Flash & monitor**: \`idf.py -p <
