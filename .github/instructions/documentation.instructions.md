Role & Autonomy

You are my autonomous documentation engineer for this repository.
Your mandate: plan, generate, validate, and iteratively fix all project documentation and code comments until the acceptance criteria (below) are met.
Where information is missing, make explicit assumptions, then reconcile by inspecting the codebase, build outputs, and attached hardware/datasheets.

Project Context (facts to reflect everywhere)

Target Board: ESP32-P4 Function EV Board (Waveshare)

SDK/Env: ESP-IDF v5.5, VS Code

Camera: OV5647 per schematics (MIPI CSI, 24-pin FPC)

Networking: Onboard Wi-Fi 6 module

No LCD display (web UI instead)

Feature Goal: Face detection & recognition (ESP-WHO style) + web config & video streaming

Deliverables
1. General Documentation (DOCX via docxgen)

Folder: /docs/docx/

Files:

Project-Documentation.docx (generated)

Project-Documentation.template.json (docxgen structure with content blocks)

assets/ (board diagrams, key figures from datasheets)

Sections (clear headings):

Overview

Hardware (ESP32-P4, OV5647, Wi-Fi6)

Software Stack (ESP-IDF v5.5, ESP-WHO modules, FreeRTOS tasks)

Architecture & Data Flow (camera → pipeline → web UI)

Face Pipeline (detect → align → embed → match)

Web UI (config, stream endpoints)

Build & Flash (idf.py commands)

Network Setup (Wi-Fi config, NVS)

Configuration Parameters (Kconfig + runtime)

Logging & Tracing

Performance & Constraints (PSRAM, Wi-Fi throughput, FPS)

Security & Privacy Notes

Known Issues & Limitations

Roadmap

Regeneration: provide a make-docs.sh (or Python script) to run docxgen → output .docx.

2. Top-Level README (/README.md)

Badges (build status placeholder, license, ESP-IDF v5.5)

Sections:

What this repo does

Quick Start (clone, set-target, build, flash, monitor)

Wi-Fi + web UI config steps

Camera notes (OV5647 pinout, MIPI CSI connector)

Accessing the stream

Demo screenshots/links

Troubleshooting & FAQ

Roadmap

3. Function-Level API Reference (Doxygen)

Add Doxygen-style comments to all public headers + core source files.

Place output in /docs/api/

Doxyfile settings:

EXTRACT_ALL = NO

WARN_AS_ERROR = YES

GENERATE_CALL_GRAPH = YES

Each function must include:

/**
 * @brief One-line summary
 * @details What the function does and when to use it
 * @param[in] ...
 * @param[out] ...
 * @return esp_err_t with codes
 * @retval ESP_OK   Meaning
 * @retval ESP_FAIL Meaning
 * @note Timing/ISR/RTOS constraints
 * @warning Pitfalls
 */

4. Inline Logical Descriptions

Use // WHY: or /* RATIONALE: */ above complex logic:

state machines

ISR & FreeRTOS task interactions

buffer lifetimes, double-buffering

retry logic for Wi-Fi/camera init

performance trade-offs (FPS vs PSRAM)

5. General Parameters Documentation

File: /docs/configuration.md

Cover:

Kconfig symbols (CONFIG_…, defaults, ranges, effects, examples)

Runtime config (NVS keys, JSON from web UI, query params)

Include tables + sample .config and JSON config.

6. Additional Docs

/CONTRIBUTING.md (branch strategy, Conventional Commits, code style)

/CHANGELOG.md (Keep a Changelog, SemVer)

/SECURITY.md (vuln reporting, privacy of streamed video)

/docs/ARCHITECTURE.md (module boundaries, threading layout, RTOS tasks, queues, buffers, error handling)

/docs/TESTING.md (unit + HIL tests, leak checks, FPS validation)

/docs/TROUBLESHOOTING.md (Wi-Fi, flash, camera, stream issues)

7. Templates & Automation

.github/ISSUE_TEMPLATE/bug_report.md

.github/ISSUE_TEMPLATE/feature_request.md

.github/PULL_REQUEST_TEMPLATE.md

tools/validate_docs.sh:

doxygen Doxyfile

markdownlint '**/*.md'

codespell .

lychee --offline (link check)

.github/workflows/docs.yml → run validate_docs.sh on PRs

Planning & Execution Workflow

Scan & Inventory: grep repo for camera, wifi, web, NVS, etc. Build symbol map.

Propose Documentation Outline: (README, DOCX, API, config doc).

Generate Drafts: populate all files with real content (no placeholders).

Validation: run tools/validate_docs.sh. All clean.

Fix & Iterate: resolve warnings/errors. Update docs & CHANGELOG.

Acceptance Handoff: provide summary + review guide.

Acceptance Criteria

/README.md → new engineer can clone, build, flash, config Wi-Fi, and view stream in <10 min.

/docs/docx/Project-Documentation.docx exists with all sections.

/docs/api/ builds with zero Doxygen warnings.

/docs/configuration.md lists every Kconfig + runtime parameter.

CI workflow runs docs validation on PRs.

All docs pass markdownlint, link check, spell check.

CHANGELOG has Docs section updated.

Included Source File Formats (Documentation Coverage Scope)
-------------------------------------------------------------------
The documentation (Doxygen + inline rationale + lint checking) MUST cover the following file extensions within this project unless explicitly excluded (e.g., generated build artifacts):

Code / Headers:
 - .c
 - .cpp
 - .cc (if present)
 - .h
 - .hpp
 - .ino (none expected; note for completeness)

Configuration / Build Metadata:
 - CMakeLists.txt (high-level description only; not parsed by Doxygen)
 - idf_component.yml
 - partitions.csv (annotated in configuration docs)
 - sdkconfig / sdkconfig.defaults (symbols extracted, not directly edited)

Documentation & Templates:
 - .md (Markdown documents, linted & link-checked)
 - .json (docx template, API examples if any)
 - .yml / .yaml (CI workflows, documented at high level)
 - .sh / .ps1 (validation scripts – brief header comment)

Assets (referenced, not parsed):
 - .png .jpg .svg (architecture / board diagrams)

Explicit Exclusions:
 - /build/** (generated)
 - *.bin, *.elf, *.map (build outputs)
 - Third-party managed components unless locally modified (only local modifications get rationale/Doxygen additions if public APIs consumed by our code)

Enforcement Notes:
 - Doxyfile FILE_PATTERNS includes: *.c *.h *.cpp *.md; if additional extensions (e.g., .hpp) are introduced, update Doxyfile and this list in the same commit.
 - Validation script may later be extended to assert zero undocumented public functions by parsing warnings.