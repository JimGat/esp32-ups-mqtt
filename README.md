# ESP32 UPS MQTT Bridge

**JimGat Lab Sentinel:** USB-HID UPS telemetry in, MQTT power intelligence out.

This firmware targets ESP32-S3 USB OTG boards connected to APC UPS USB HID ports. It publishes UPS status and metrics to MQTT for power-outage alerting, outage/recovery tracking, brownout monitoring, and low-voltage trend collection. It began as a clean public import and extension of [`hms-homelab/hms-esp-apc`](https://github.com/hms-homelab/hms-esp-apc) by Albin Amat, with attribution preserved but without carrying upstream local configuration history into this repo.

## Current Status: v0.3.19-dev
- **Target Hardware**: ESP32-S3 (USB OTG on GPIO19=D-, GPIO20=D+)
- **UPS Support**: APC Back-UPS (fully working) and Smart-UPS SMT2200 (VID:PID 051D:0003).
- **MQTT Path**: `homeassistant/sensor/ups_bridge` / `ups_bridge/<device_id>/events/power`
- **Key Features**:
  - Raw binary Web OTA (no FormData corruption).
  - SNTP time sync initialized post-WiFi connection.
  - Cache-busted `/system` endpoint for accurate uptime display.
  - 2000ms USB timeouts to prevent SMT2200 "late callback" memory leaks.
- **USB Debug Mode**: Web UI-controlled, runtime-only HID debugger for descriptor dumps and manual GET_REPORT probing.
- **Pending**: Dynamic HID Report Descriptor parsing for SMT2200 Input Voltage & Load. (See *Protocol Extraction* below).

## JimGat Fork Changes
- Uses ESP-IDF/CMake, not Arduino.
- Removes compiled-in Wi-Fi and MQTT credentials.
- Stores Wi-Fi and MQTT settings in ESP32 NVS.
- Starts a first-boot/fallback provisioning AP when settings are missing or Wi-Fi connection fails.
- Keeps the upstream web configuration/status UI and saves submitted settings to NVS.
- **Hardware-agnostic naming**: Standardized MQTT topics to `ups_bridge` instead of `apc_ups`.

## First Boot Provisioning
1. Flash the firmware.
2. On first boot, connect to the Wi-Fi AP named `ESP32-UPS-Setup-XXXXXX`.
3. Use provisioning AP password `configureme` unless changed in `idf.py menuconfig`.
4. Open `http://192.168.4.1/`.
5. Log in to the web interface with username `admin` and the current config AP/admin password, then enter Wi-Fi SSID/password, MQTT broker URL, optional MQTT username/password, optional device label, optional new config AP/admin password, and publish interval.
6. Save. The device stores settings in NVS and reboots into station mode.

*No site Wi-Fi or MQTT credentials are stored in source, `sdkconfig.defaults`, or firmware defaults.*

## Immediate Power-Event MQTT Messages
The device keeps the full voltage/load/battery telemetry publish interval at the configured general status cadence (default `60` seconds). Power-state changes **do not** wait for that interval.

When the UPS status changes, firmware immediately publishes a compact snapshot plus an event JSON message:

| Event | Trigger |
|-------|---------|
| `power_lost` | Line power changes from online to on-battery/discharging |
| `power_restored` | UPS returns from on-battery/discharging to online |
| `low_battery` | UPS low-battery flag asserts |
| `low_battery_cleared` | UPS low-battery flag clears |

Dedicated event topic:
```text
ups_bridge/<device_id>/events/power
```

Example event payload:
```json
{
  "event": "power_lost",
  "device_id": "ups_bridge_9c139eabd09c",
  "uptime_ms": 123456,
  "status": "On Battery",
  "online": false,
  "discharging": true,
  "low_battery": false,
  "input_voltage": 0.00,
  "battery_charge": 98.00,
  "battery_runtime_seconds": 2400,
  "load_percent": 23.00
}
```


## USB Debug Mode

Open `http://<device-ip>/usb-debug` after logging in. This page turns the bridge into a small, read-only USB HID lab without disabling Wi-Fi, HTTP, logs, or Web OTA.

Modes:
- **Normal Bridge**: default after every boot. MQTT telemetry and power-event monitoring run normally.
- **Passive Capture**: captures raw interrupt-IN reports for inspection without mutating production metrics.
- **Active Debug**: pauses normal bridge USB reads/polls and processes queued debug commands from the Web UI.

Supported first-pass commands:
- HID Report Descriptor dump (`GET_DESCRIPTOR`, type `0x22`)
- Manual HID `GET_REPORT` with selectable report type, Report ID, and response length
- Debug record capture view with hex output

Safety rules:
- Debug mode is runtime-only and is not stored in NVS. Reboot always returns to Normal Bridge.
- This first implementation is read-only. It does not expose `SET_REPORT` or UPS control commands.
- HTTP handlers only enqueue debug commands; the USB host task owns all USB transfers so ESP-IDF USB context rules are preserved.

## Browser Web Flasher
> **Flash from your browser:** [Open the JimGat Lab UPS MQTT Bridge Web Flasher](https://jimgat.github.io/esp32-ups-mqtt/)
>
> Use desktop Chrome or Edge with Web Serial enabled.

This repo includes a Dexter-lab themed browser flasher under `docs/`, adapted from the CYM-NM28C5 web flasher pattern. It is intended for GitHub Pages and desktop Chrome/Edge with Web Serial enabled.

Flash package layout:
| File | Offset | Purpose |
|------|--------|---------|
| `binaries-esp32s3/bootloader.bin` | `0x0` | ESP32-S3 bootloader |
| `binaries-esp32s3/partition-table.bin` | `0x8000` | Partition table |
| `binaries-esp32s3/ota_data_initial.bin` | `0xD000` | OTA initial data |
| `binaries-esp32s3/esp32_ups_mqtt_bridge.bin` | `0x20000` | Main firmware app |

GitHub Pages deployment is handled by `.github/workflows/deploy-flasher.yml`. In GitHub, set Pages source to **GitHub Actions**.

---
*(Original upstream README preserved below for reference)*
---

# hms-esp-apc (Upstream Reference)

ESP32-S3 USB Host to MQTT bridge for APC UPS with Home Assistant auto-discovery.

Reads real-time metrics from an APC Back-UPS over USB HID and publishes them to an MQTT broker. Home Assistant discovers the UPS automatically — no YAML configuration required.

## Features
- USB HID host communication with APC UPS (no apcupsd/NUT required)
- MQTT publishing with Home Assistant MQTT auto-discovery
- Unique device ID per bridge (based on ESP32 MAC address)
- 30+ sensor entities: battery, input power, load, status, timers, and more
- Automatic reconnection for both WiFi and MQTT
- Fallback to simulated data when no USB device is connected (for development)
- 10-second boot delay window for reflashing

## Supported Hardware
- **Microcontroller**: ESP32-S3 with USB OTG (e.g., M5Stack AtomS3, ESP32-S3-DevKitC)
- **UPS**: APC UPS (USB VID `051D`, PID `0002` Back-UPS or `0003` Smart-UPS) — tested with Back-UPS XS 1000M, Smart-UPS SMT2200 / C 1500
- **USB Connection**: USB OTG on GPIO19 (D-) / GPIO20 (D+)

### Wiring
Connect the APC UPS USB port to the ESP32-S3 USB OTG pins:
| UPS USB | ESP32-S3 |
|---------|----------|
| D- (white) | GPIO19 |
| D+ (green) | GPIO20 |
| VCC (red) | 5V |
| GND (black) | GND |

> **Note**: Most ESP32-S3 dev boards expose the USB OTG pins on a dedicated connector. If your board uses the USB OTG port for programming (USB-CDC), you may need a USB hub or OTG adapter.

## Protocol Extraction (SMT2200 Pending Task)
The Smart-UPS SMT2200 uses different HID Report IDs for Input Voltage and Percent Load than consumer Back-UPS models. Hardcoded IDs (`0x31`, `0x50`) will STALL on this device. 

To finalize 100% telemetry, extract the raw HID Report Descriptor from a Linux machine:
```bash
sudo apt install usbutils
sudo usbhid-dump -m 051d -e stream | grep -v : | xxd -r -p | hexdump -C
```
Paste the hex output into the project issue tracker to decode the exact Report IDs and bit-offsets for dynamic parsing.

## Architecture
The firmware runs four FreeRTOS tasks:
1. **USB Host Task** — Manages the USB host stack, receives interrupt transfers (automatic status updates from UPS), and polls feature reports (voltage, load, thresholds) on a configurable interval.
2. **MQTT Publish Task** — Publishes Home Assistant MQTT discovery configs on startup, then periodically reads the shared metrics struct and publishes all sensor values.
3. **WiFi Manager** — Handles WiFi STA connection with automatic reconnection on disconnect.
4. **Main / app_main** — Initializes NVS, WiFi, MQTT, SNTP, and USB host. Creates all tasks and enters idle.

## Home Assistant Entities
Once running, the following sensors appear automatically in Home Assistant under a device named **UPS Bridge (MAC)**. See the full upstream list in the original `hms-esp-apc` documentation for all 30+ entities.

## Known Hardware Limitations
- **Input frequency**: The APC Back-UPS XS 1000M reports 0 Hz for input frequency — this appears to be a hardware limitation.
- **Output voltage**: Line-interactive UPS models do not measure output voltage separately; it mirrors input voltage.
- **Battery date encoding**: The manufacture date is reported as a raw day count; decoding varies by model.

## License
This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.
