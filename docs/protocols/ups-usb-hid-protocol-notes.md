# UPS USB HID Protocol Notes

This document is the seed for manufacturer/model-specific UPS protocol tables used by the ESP32 UPS MQTT Bridge.

The goal is to stop hardcoding one UPS family's HID report IDs into all models. Each detected manufacturer/model should eventually select a small protocol profile: known VID/PID, expected HID reports, scaling rules, status bit layout, and model-specific caveats.

## Table Schema Draft

Recommended fields for each protocol profile:

| Field | Purpose |
|---|---|
| `manufacturer` | Human-readable manufacturer name, e.g. `APC` |
| `model_family` | Broad compatibility family, e.g. `Smart-UPS HID` |
| `model` | Specific tested model, e.g. `SMT2200` |
| `vid` / `pid` | USB VID/PID used for detection |
| `descriptor_prefix_skip` | Bytes to strip from ESP32 debug transfer records before descriptor decode. Usually `8` for control SETUP packet artifact, `0` for pure descriptor files. |
| `status_report_id` | Report ID carrying online/on-battery/charging/discharging flags |
| `runtime_report_id` | Report ID carrying estimated runtime, if confirmed |
| `battery_charge_report_id` | Remaining capacity / battery charge report |
| `battery_voltage_report_id` | Battery voltage report |
| `input_voltage_report_id` | Input voltage report, if confirmed |
| `load_percent_report_id` | Load percent report, if confirmed |
| `beeper_report_id` | Audible alarm / beeper control/status report |
| `vendor_reports` | Vendor-page report IDs to retain for future decoding |
| `unsupported_reports` | Known report IDs that stall or are wrong for this model |
| `confidence` | `confirmed`, `likely`, or `tentative` |
| `notes` | Human-readable caveats and sample payloads |

## APC Smart-UPS SMT2200

| Field | Value |
|---|---|
| Manufacturer | APC / Schneider Electric |
| Model | Smart-UPS SMT2200 |
| USB VID:PID | `051D:0003` |
| Project target | Jim's desk UPS bridge |
| Descriptor source | ESP32 `/usb-debug` descriptor dump, `usbdump.txt` |
| Dump status | Valid descriptor dump, but first v0.3.20 capture included the 8-byte setup packet and likely missed the final 8 descriptor bytes. v0.3.21+ strips setup packet for clean comparison by default; v0.3.22+ can optionally include the raw setup packet for deep USB debugging. v0.3.23+ requests 1024 bytes because the SMT2200 descriptor continues past 512 bytes; clean capture confirms the full payload is 515 bytes / raw control transfer is 523 bytes. |
| HID descriptor start | `05 84 09 04 A1 01 ...` |
| Family | HID Power Device / Battery System, Smart-UPS variant |

### Important Capture Note

The v0.3.20 ESP32 debug output included this 8-byte USB control SETUP packet before the actual descriptor payload:

```text
81 06 00 22 00 00 00 02
```

That means:

| Byte(s) | Meaning |
|---|---|
| `81` | IN, Standard, Interface |
| `06` | GET_DESCRIPTOR |
| `00 22` | Descriptor type `0x22`, HID Report Descriptor |
| `00 00` | Interface 0 |
| `00 02` | Requested length `0x0200` / 512 bytes |

For HID parsing, strip these 8 bytes. The real report descriptor begins at:

```text
05 84 09 04 A1 01
```


### ESP32 Debug Views

Starting in v0.3.22-dev, `/usb-debug` has two descriptor dump views:

- **Payload-only** (default): strips the 8-byte USB control SETUP packet and starts at the real HID descriptor (`05 84 ...`). This is best for protocol decoding and Linux `usbhid-dump` comparison.
- **Raw-control** (optional checkbox): includes the 8-byte SETUP packet (`81 06 00 22 00 00 00 02`) before the descriptor payload. This is best for debugging ESP-IDF USB control-transfer behavior.

### Confirmed / Likely Reports

| Report ID | Direction | HID Usage / Meaning | Mapping | Confidence | Notes |
|---:|---|---|---|---|---|
| `0x0C` | Input + Feature | Remaining Capacity | `battery_charge` | Confirmed descriptor, confirmed runtime behavior | Likely payload byte after report ID is percent. |
| `0x0D` | Input + Feature | Voltage-like Battery System value | `battery_voltage` | Confirmed descriptor, confirmed runtime behavior | Unit suggests volts. Use live payload samples to confirm scale. |
| `0x0A` | Feature | Runtime/config-style value | `battery_runtime` currently | Likely | Existing parser saw `0A C0 12` as 4800 seconds / 80 min. Keep mapping but verify descriptor semantics/scaling. |
| `0x09` | Input + Feature bitfield plus 16-bit values | PresentStatus-style flags and related status/threshold fields | `status`, `power_failure`, flags, possible load flag | Likely/needs bit confirmation | Clean v0.3.22 dump shows dense 1-bit PresentStatus-style fields under report `0x09`, not `0x07`. Includes AC/line-present-like usage, Battery Present, Overload, Shutdown Requested, Charging, Discharging, Need Replacement, Below Remaining Capacity Limit, Percent Load usage/flag, and vendor bit. |
| `0x14` | Input + Feature-like | Audible Alarm Control | `beeper_status` | Likely | 8-bit enum, logical range 1..3. Do not expose writes until SET_REPORT is deliberately implemented and gated. |
| `0x11` | Feature | Capacity / low-charge threshold style | `battery_charge_low` | Likely | Prior live debug showed `11 0A` = 10%. |

### Vendor Reports to Preserve

These appear on vendor page `0xFF86` and should be captured for future decoding, not discarded:

| Report ID | Direction | Size | Notes |
|---:|---|---:|---|
| `0x89` | Input | 63 bytes | Vendor input report, usage `0xFD`; Feature GET_REPORT correctly returns `ESP_ERR_NOT_SUPPORTED` because it is not a Feature report. |
| `0x90` | Output | 63 bytes | Vendor output report, usage `0xFC`; do not use until write operations are intentionally supported. |
| `0x96` | Feature | 63 bytes | Vendor feature report, usage `0xF1` |
| `0x8D` | Feature | 2 bytes | Vendor feature report, usage `0xF7` |
| `0x8E` | Feature | 2 bytes | Vendor feature report, usage `0xF6` |
| `0x93` | Feature | 2 bytes | Vendor feature report, usage `0xF3` |
| `0x94` | Feature | 2 bytes | Vendor feature report, usage `0xF2` |
| `0x92` | Feature | 2 bytes | Vendor feature report, usage `0xF4`; confirmed complete in v0.3.23 clean dump (`85 92 09 F4 15 00 26 FF 00 75 08 95 02 B1 23`). |

### Known Bad / Not Universal

| Report ID | Old Meaning | SMT2200 Status | Notes |
|---:|---|---|---|
| `0x31` | Back-UPS input voltage | Do not assume / likely stalls | This came from Back-UPS assumptions and should not be used for SMT2200 without descriptor confirmation. |
| `0x50` | Back-UPS load percent | Do not assume / likely stalls | SMT2200 descriptor points toward status/report `0x07` and/or vendor reports for load-related data. |

### Status Bitfield Hypothesis for Report `0x09`

Report `0x09` appears to be a packed one-bit PresentStatus-style report in the clean v0.3.22 descriptor dump. Earlier `0x07` notes came from a malformed/truncated parse and should be ignored for SMT2200. Descriptor-derived usage order suggests the firmware should decode cautiously and expose uncertain bits as raw flags until confirmed with live power events.

Likely flags include:

| Usage | Meaning |
|---|---|
| AC Present | Online / line power present |
| Battery Present | Battery installed/present |
| Overload | UPS overloaded |
| Shutdown Requested | UPS has requested shutdown |
| Charging | Battery charging |
| Discharging | On battery / battery discharging |
| Need Replacement | Replace battery |
| Below Remaining Capacity Limit | Low battery / below threshold |

### Descriptor Length Discovery

The clean v0.3.22 payload-only dump returned exactly 512 descriptor bytes and ended mid-HID-item (`... 85 92 09 F4 15 00 26 FF 00 75 08 95`). v0.3.23-dev increased the requested descriptor payload to 1024 bytes. The follow-up clean capture completed the descriptor at 515 payload bytes / 523 raw-control bytes. The final tail is `B1 23 C0`, completing vendor Feature report `0x92` and the closing collection.


### Confirmed Online GET_REPORT Samples

Captured from Jim's APC Smart-UPS SMT2200 while online / line power present, using `/usb-debug` Active Debug mode and HID Feature GET_REPORT (`report_type=3`). These are sample payloads for building the SMT2200 model profile.

| Report ID | Raw Response | Decoded Raw | Current Interpretation | Confidence |
|---:|---|---:|---|---|
| `0x09` | `09 A8 4A` | `0x4AA8` / 19112 | PresentStatus-style bitfield; bits set: 3, 5, 7, 9, 11, 14 while online | Confirmed sample, bit meanings need offline compare |
| `0x0C` | `0C 64` | 100 | Battery charge / RemainingCapacity = 100% | Confirmed |
| `0x0D` | `0D B0 13` | 5040 | Battery voltage-like value. Descriptor unit/exponent suggests special scaling; compare against existing bridge reading before final scale. | Confirmed sample, scale pending |
| `0x0A` | `0A C0 12` | 4800 | Runtime = 4800 seconds = 80 minutes | Confirmed |
| `0x0B` | `0B 4A 15` | 5450 | Voltage usage report. Possible nominal/config voltage or model-specific value; scale pending. | Confirmed sample, meaning pending |
| `0x11` | `11 0A` | 10 | Low charge threshold = 10% | Confirmed |
| `0x14` | `14 02` | 2 | Audible alarm / beeper enum = 2 | Confirmed sample, enum labels pending |


### Confirmed Vendor / Alternate GET_REPORT Samples

Captured from Jim's APC Smart-UPS SMT2200 while online / line power present.

| Request | Result | Decoded Raw | Notes |
|---|---|---:|---|
| Input `0x09`, len 16 | `09 A8 4A` | `0x4AA8` / 19112 | Same value as Feature `0x09`; confirms report `0x09` is available as both Input and Feature and can be used for status. Bits set online: 3, 5, 7, 9, 11, 14. |
| Feature `0x89`, len 64 | `ESP_ERR_NOT_SUPPORTED` | n/a | Descriptor marks `0x89` as vendor Input, not Feature. Try report type `1` if needed. |
| Feature `0x96`, len 64 | `96 00` | 0 | Vendor Feature usage `0xF1`; currently zero while online. |
| Feature `0x8D`, len 8 | `8D 00 00` | 0 | Vendor Feature usage `0xF7`; currently zero. |
| Feature `0x8E`, len 8 | `8E 00 00` | 0 | Vendor Feature usage `0xF6`; currently zero. |
| Feature `0x92`, len 8 | `92 03 00` | 3 | Vendor Feature usage `0xF4`; nonzero online sample. |
| Feature `0x93`, len 8 | `93 01 00` | 1 | Vendor Feature usage `0xF3`; nonzero online sample. |
| Feature `0x94`, len 8 | `94 01 00` | 1 | Vendor Feature usage `0xF2`; nonzero online sample. |


### Confirmed Input GET_REPORT Samples

Captured from Jim's APC Smart-UPS SMT2200 while online / line power present.

| Request | Result | Decoded Raw | Notes |
|---|---|---:|---|
| Input `0x89`, len 64 | `ESP_ERR_NOT_SUPPORTED` | n/a | Although descriptor declares vendor Input `0x89`, this device/stack does not support manual Input GET_REPORT for it. It may only arrive by interrupt/event, or require APC vendor protocol behavior. |
| Input `0x0D`, len 8 | `0D B0 13` | 5040 | Same as Feature `0x0D`; voltage-like report is accessible as Input and Feature. |
| Input `0x0B`, len 8 | `0B 4A 15` | 5450 | Same as Feature `0x0B`; voltage usage report is accessible as Input and Feature. |

### Firmware Profile Implementation

v0.3.24-dev adds config-selectable profiles:

| Profile | Behavior |
|---|---|
| Auto Detect | Best-effort. APC VID:PID `051D:0003` resolves to APC Smart-UPS SMT2200; otherwise Generic APC HID. |
| APC Smart-UPS SMT2200 | Polls confirmed reports `0x09`, `0x0C`, `0x0A`, `0x0D`, `0x0B`, `0x11`, `0x14` and profile-gates parsing for `0x09` status and `0x14` beeper. |
| Generic APC HID | Preserves earlier generic/backward-compatible APC polling and decoding. |

SMT2200 fields still marked scale/bit-pending are intentionally conservative in the web/MQTT layer until more state-change captures are available.

### Next Capture Tasks

1. Preserve the v0.3.23 clean descriptor dump as the APC SMT2200 baseline: payload 515 bytes, raw 523 bytes, payload-only view.
2. Descriptor length/tail confirmed: 515-byte payload, final report `0x92`, closing `C0`.
3. Manually GET_REPORT likely IDs and preserve sample payloads:
   - `0x09` status bitfield / PresentStatus flags
   - `0x0C` battery charge
   - `0x0D` battery voltage
   - `0x0A` runtime
   - `0x11` low charge threshold
   - `0x14` beeper/audible alarm
   - vendor IDs `0x89`, `0x96`, `0x8D`, `0x8E`, `0x92`, `0x93`, `0x94`
4. Compare status report before/after a controlled line-power pull if safe.
5. Only after read-only mappings are confirmed, consider model-specific write/SET_REPORT support.
