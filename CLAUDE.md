# CLAUDE.md — UPS MQTT Bridge

This file provides guidance to Claude Code (claude.ai/code) and AI agents working on this repository.

## Project

**ESP32 UPS MQTT Bridge** — USB HID to MQTT bridge for APC UPS devices with Home Assistant auto-discovery.
Build system: **ESP-IDF 6.0**. Target chip: **ESP32-S3**.

## Build & Flash

```bash
cd /home/dev/projects/esp32-ups-mqtt
source /home/dev/esp/esp-idf/export.sh   # must be done each shell session
idf.py build
```

There are no tests. The only validation is a successful `idf.py build`.

## Git & Branch Rules — CRITICAL

| Branch | Purpose | Who can push |
|--------|---------|-------------|
| `dev` | Active development, all code changes, binary updates | AI agents and developers |
| `main` | Release branch only — stable firmware + deployed web flasher | **Jim only** (during release) |

**NEVER push to `main` unless Jim explicitly says "we are doing a release."**

All development work goes on `dev`:
1. Code changes committed to `dev`
2. Build and test on `dev`
3. Update binaries in `binaries-esp32s3/` on `dev`
4. When Jim says release: merge `dev` into `main`, push `main`

## Web Flasher Deployment

The web flasher at https://jimgat.github.io/esp32-ups-mqtt/ deploys from `main` via GitHub Actions.
The workflow (`.github/workflows/deploy-flasher.yml`) triggers on push to `main` when `binaries-esp32s3/*.bin` or `docs/**` change.

**Since we only push to `main` on release, the flasher only updates on release.** This is correct behavior.

For dev-cycle testing, use the local flasher:
```bash
cd /home/dev/projects/esp32-ups-mqtt
python3 -m http.server 8000
# Open http://localhost:8000/docs/ in Chrome/Edge on a machine with Web Serial access.
```

## Post-Build Binary Copy

CMakeLists.txt automatically copies binaries to `binaries-esp32s3/` after every build.
These should be committed on `dev` so the latest dev firmware is always in the repo.

## Supported Hardware

- **Microcontroller**: ESP32-S3 with USB OTG
- **UPS**: APC UPS (USB VID 0x051D, PID 0x0002 Back-UPS or 0x0003 Smart-UPS)
- **Tested models**: Back-UPS XS 1000M, Smart-UPS C 1500

## Architecture

- `main/usb_host_manager.c` — USB HID host: interrupt transfers + GET_REPORT feature polls
- `main/apc_hid_parser.c` — HID report parser (currently model-specific, needs dynamic descriptor parsing)
- `main/mqtt_manager.c` — MQTT publishing with HA auto-discovery
- `main/http_server.c` — Web UI for configuration and monitoring
- `main/main.c` — Application entry, task orchestration

## HID Report Map (Back-UPS XS 1000M)

| Report ID | Type | Content | Encoding |
|-----------|------|---------|----------|
| 0x0C | Input | Battery charge (+ runtime if 4+ bytes) | data[1] = % |
| 0x0D | Input | Battery voltage (interrupt variant) | encoding TBD |
| 0x09 | Feature | Battery voltage (polled) | 16-bit LE / 100 |
| 0x16 | Feature | Present status bits | bit flags |
| 0x31 | Feature | Input voltage | 16-bit LE |
| 0x50 | Feature | Load percentage | 8-bit |
