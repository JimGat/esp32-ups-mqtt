# esp32-apc-ups-mqtt

JimGat clean-room public import and extension of [`hms-homelab/hms-esp-apc`](https://github.com/hms-homelab/hms-esp-apc).

This firmware targets ESP32-S3 USB OTG boards connected to APC UPS USB HID ports. It publishes UPS status and metrics to MQTT for power-outage alerting, outage/recovery tracking, brownout monitoring, and low-voltage trend collection.

## JimGat fork changes

- Uses ESP-IDF/CMake, not Arduino.
- Removes compiled-in Wi-Fi and MQTT credentials.
- Stores Wi-Fi and MQTT settings in ESP32 NVS.
- Starts a first-boot/fallback provisioning AP when settings are missing or Wi-Fi connection fails.
- Keeps the upstream web configuration/status UI and saves submitted settings to NVS.

## First boot provisioning

1. Flash the firmware.
2. On first boot, connect to the Wi-Fi AP named `APC-UPS-Setup-XXXXXX`.
3. Use provisioning AP password `configureme` unless changed in `idf.py menuconfig`.
4. Open `http://192.168.4.1/`.
5. Enter Wi-Fi SSID/password, MQTT broker URL, optional MQTT username/password, and publish interval.
6. Save. The device stores settings in NVS and reboots into station mode.

No site Wi-Fi or MQTT credentials are stored in source, `sdkconfig.defaults`, or firmware defaults.

## Upstream README

# hms-esp-apc

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-support-%23FFDD00.svg?logo=buy-me-a-coffee)](https://www.buymeacoffee.com/aamat09)

ESP32-S3 USB Host to MQTT bridge for APC UPS with Home Assistant auto-discovery.

Reads real-time metrics from an APC Back-UPS over USB HID and publishes them to an MQTT broker. Home Assistant discovers the UPS automatically — no YAML configuration required.

## Features

- USB HID host communication with APC Back-UPS (no apcupsd/NUT required)
- MQTT publishing with Home Assistant MQTT auto-discovery
- Unique device ID per bridge (based on ESP32 MAC address)
- 30+ sensor entities: battery, input power, load, status, timers, and more
- Automatic reconnection for both WiFi and MQTT
- Fallback to simulated data when no USB device is connected (for development)
- 10-second boot delay window for reflashing

## Supported Hardware

- **Microcontroller**: ESP32-S3 with USB OTG (e.g., M5Stack AtomS3, ESP32-S3-DevKitC)
- **UPS**: APC UPS (USB VID `051D`, PID `0002` Back-UPS or `0003` Smart-UPS) — tested with Back-UPS XS 1000M, Smart-UPS C 1500
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

## Prerequisites

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) v5.3 or later
- An MQTT broker (e.g., Mosquitto)
- Home Assistant with MQTT integration enabled

## Build & Flash

```bash
# Clone the repository
git clone https://github.com/JimGat/esp32-apc-ups-mqtt.git
cd esp32-apc-ups-mqtt

# Build. Do not put site Wi-Fi/MQTT credentials in source or sdkconfig.
idf.py build

# Flash (replace PORT with your serial port)
idf.py -p PORT flash

# Monitor serial output
idf.py -p PORT monitor
```

## Configuration

Wi-Fi and MQTT settings are provisioned at runtime and stored in NVS, not hard-coded into source or sdkconfig.

On first boot, or when the configured Wi-Fi cannot be reached, the device starts a setup AP named `APC-UPS-Setup-XXXXXX`. Connect to it and browse to `http://192.168.4.1/` to save:

| Setting | Storage | Description |
|---------|---------|-------------|
| WiFi SSID | NVS | Wi-Fi network name |
| WiFi Password | NVS | Wi-Fi password |
| MQTT Broker URL | NVS | MQTT broker address, e.g. `mqtt://192.168.1.100` |
| MQTT Username | NVS | MQTT username, optional |
| MQTT Password | NVS | MQTT password, optional |
| Publish Interval | NVS | How often to publish metrics to MQTT |

Build-time menuconfig only controls non-site defaults:

| Setting | Default | Description |
|---------|---------|-------------|
| Provisioning AP Prefix | `APC-UPS-Setup` | First-boot/fallback setup AP SSID prefix |
| Provisioning AP Password | `configureme` | Temporary setup AP password; change for production images if desired |
| UPS Poll Interval | `5000` ms | How often to poll feature reports from the UPS |
| MQTT Publish Interval | `60000` ms | Initial publish interval before provisioning overrides it |

## Home Assistant Entities

Once running, the following sensors appear automatically in Home Assistant under a device named **APC UPS (MAC)**:

### Battery
| Entity | Unit | Description |
|--------|------|-------------|
| Battery Charge | % | Current battery charge level |
| Battery Voltage | V | Current battery voltage |
| Battery Nominal Voltage | V | Configured battery voltage (e.g., 12V) |
| Battery Runtime | s | Estimated runtime on battery |
| Battery Low Runtime | s | Low runtime threshold |
| Battery Low Charge | % | Low charge threshold |
| Battery Warning Charge | % | Warning charge threshold |
| Battery Type | — | Chemistry (e.g., PbAc) |
| Battery Manufacture Date | — | Battery manufacture date |

### Input Power
| Entity | Unit | Description |
|--------|------|-------------|
| Input Voltage | V | Current input (mains) voltage |
| Input Nominal Voltage | V | Configured nominal voltage (e.g., 120V) |
| Low Voltage Transfer | V | Switch-to-battery below this voltage |
| High Voltage Transfer | V | Switch-to-battery above this voltage |
| Input Sensitivity | — | Sensitivity setting (low/medium/high) |
| Last Transfer Reason | — | Why the UPS last switched to battery |

### Output / Load
| Entity | Unit | Description |
|--------|------|-------------|
| Load | % | Current load as percentage of capacity |
| Nominal Power | W | Rated power capacity (e.g., 600W) |

### Status & Timers
| Entity | Unit | Description |
|--------|------|-------------|
| UPS Status | — | OL (online), OB (on battery), CHRG, LB, etc. |
| Beeper Status | — | enabled / disabled / muted |
| Reboot Delay | s | Configured delay before reboot |
| Reboot Timer | s | Active reboot countdown (-1 = inactive) |
| Shutdown Timer | s | Active shutdown countdown (-1 = inactive) |
| Self-Test Result | — | Last self-test outcome |

### Device Info
| Entity | Description |
|--------|-------------|
| Driver Name | `esp32-usb-hid` |
| Driver Version | Driver version string |
| Driver State | `running` |
| Power Failure | `OK` or failure reason |

## Architecture

The firmware runs four FreeRTOS tasks:

1. **USB Host Task** — Manages the USB host stack, receives interrupt transfers (automatic status updates from UPS), and polls feature reports (voltage, load, thresholds) on a configurable interval.

2. **MQTT Publish Task** — Publishes Home Assistant MQTT discovery configs on startup, then periodically reads the shared metrics struct and publishes all sensor values.

3. **WiFi Manager** — Handles WiFi STA connection with automatic reconnection on disconnect.

4. **Main / app_main** — Initializes NVS, WiFi, MQTT, and USB host. Creates all tasks and enters idle.

### Data Flow

```
APC UPS (USB Device)
  |
  | USB HID Reports (interrupt + feature)
  v
USB Host Manager ──> APC HID Parser ──> ups_metrics_t (shared struct)
                                              |
                                              v
                                    MQTT Publish Task ──> MQTT Broker ──> Home Assistant
```

## Known Hardware Limitations

- **Input frequency**: The APC Back-UPS XS 1000M reports 0 Hz for input frequency — this appears to be a hardware limitation.
- **Output voltage**: Line-interactive UPS models do not measure output voltage separately; it mirrors input voltage.
- **Firmware version**: Not available via HID reports on this UPS model.
- **Battery date encoding**: The manufacture date is reported as a raw day count; decoding varies by model.

## Contributing

Contributions are welcome! Please open an issue or pull request.

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/my-change`)
3. Commit your changes
4. Push to the branch and open a Pull Request
---

## ☕ Support

If this project is useful to you, consider buying me a coffee!

[![Buy Me A Coffee](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/aamat09)

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.
