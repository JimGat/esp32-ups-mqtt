# UPS Manufacturer / Model Profile Table Design

This document defines how UPS manufacturer/model protocol tables should be implemented as the bridge grows beyond one APC unit.

The short version: never hardcode HID report meanings globally. Every report ID, bit offset, scale, confidence level, and unsupported-report note belongs to a manufacturer/model profile. The active profile controls both the USB polling list and parser interpretation.

## Why Profiles Exist

USB HID UPS devices reuse report IDs in model-specific ways. A report that means voltage on one APC model can mean a status bitfield on another. Some reports appear in descriptors but reject manual `GET_REPORT`. Some values are exposed only by interrupt-IN packets. Some fields, like input voltage or load percent, may simply not be directly reported by a model.

A profile table prevents three failure modes:

1. Polling report IDs that constantly STALL on a specific UPS.
2. Publishing wrong values because another model's scaling was assumed.
3. Showing web/Home Assistant fields as supported when the device has not proven it can report them.

## Current Profiles

Implemented in v0.3.24-dev:

| Profile | Purpose |
|---|---|
| `Auto Detect` | Best-effort profile selection from USB VID/PID and future descriptor fingerprints. |
| `APC Smart-UPS SMT2200` | Confirmed profile for Jim's APC SMT2200, VID:PID `051D:0003`. |
| `Generic APC HID` | Conservative fallback preserving the older generic APC HID behavior. |

## Desired Long-Term Shape

Today the profile definitions are deliberately small and C-native. As the table grows, keep the same concept but move toward a declarative table shape.

Recommended future structure:

```c
typedef enum {
    UPS_FIELD_STATUS,
    UPS_FIELD_BATTERY_CHARGE,
    UPS_FIELD_BATTERY_RUNTIME,
    UPS_FIELD_BATTERY_VOLTAGE,
    UPS_FIELD_INPUT_VOLTAGE,
    UPS_FIELD_LOAD_PERCENT,
    UPS_FIELD_LOW_CHARGE_THRESHOLD,
    UPS_FIELD_BEEPER_STATUS,
    UPS_FIELD_VENDOR_PRESERVE,
} ups_field_t;

typedef enum {
    UPS_REPORT_INPUT = 1,
    UPS_REPORT_OUTPUT = 2,
    UPS_REPORT_FEATURE = 3,
} ups_report_type_t;

typedef enum {
    UPS_CONFIRMED,
    UPS_LIKELY,
    UPS_TENTATIVE,
    UPS_UNSUPPORTED,
} ups_confidence_t;

typedef struct {
    ups_field_t field;
    uint8_t report_id;
    ups_report_type_t report_type;
    uint8_t payload_offset;      // offset after report ID byte
    uint8_t width_bytes;         // 1, 2, 3, 4, or bitfield width
    bool little_endian;
    int8_t decimal_exponent;     // e.g. -2 means raw / 100
    float multiplier;            // optional post-scale multiplier
    const char *unit;
    ups_confidence_t confidence;
    const char *notes;
} ups_report_mapping_t;

typedef struct {
    ups_field_t field;
    uint8_t report_id;
    ups_report_type_t report_type;
    uint8_t bit_index;
    const char *true_label;
    const char *false_label;
    ups_confidence_t confidence;
    const char *notes;
} ups_status_bit_mapping_t;

typedef struct {
    ups_profile_t profile;
    const char *manufacturer;
    const char *model;
    uint16_t usb_vid;
    uint16_t usb_pid;
    const char *descriptor_fingerprint;
    const uint8_t *poll_reports;
    size_t poll_report_count;
    const ups_report_mapping_t *reports;
    size_t report_count;
    const ups_status_bit_mapping_t *status_bits;
    size_t status_bit_count;
    const uint8_t *known_unsupported_reports;
    size_t unsupported_report_count;
} ups_model_profile_t;
```

Do not add this entire abstraction prematurely if only two profiles exist. Use it as the target shape when the third or fourth concrete model is added.

## Minimum Data Required Before Adding a Model Profile

A model profile should not be marked `confirmed` until it has:

1. Manufacturer and model label.
2. USB VID:PID.
3. HID report descriptor dump.
4. Descriptor payload length and descriptor start/tail bytes.
5. Poll report list that has been tested for success vs STALL.
6. At least one online sample for each primary field.
7. Status report bitfield sample while online.
8. If practical, a second status sample during on-battery or fault state.
9. Known unsupported reports documented.
10. Confidence level per field.

If only descriptor data exists, add a `tentative` or `likely` profile note, not a production parser mapping.

## Confidence Levels

Use confidence per field, not just per model.

| Level | Meaning | Web/MQTT behavior |
|---|---|---|
| `confirmed` | Descriptor and real GET_REPORT/interrupt sample agree with expected physical value. | Safe to publish normally. |
| `likely` | Descriptor indicates field and sample looks plausible, but no independent validation yet. | Publish only if value is sane; document scale as pending. |
| `tentative` | We have a plausible guess, usually from one state sample. | Do not use for critical events without clear labeling. |
| `unsupported` | Device stalls, never reports it, or descriptor lacks it. | Do not show as a supported feature; avoid publishing zero as if real. |

## Auto Detect Rules

Auto Detect should resolve profiles in this order:

1. Exact VID:PID plus descriptor fingerprint match.
2. Exact VID:PID plus known model/string descriptor match, if available.
3. Exact VID:PID family match to a known safe profile.
4. Manufacturer-family generic fallback.
5. Unknown/Generic HID fallback.

For APC SMT2200 today:

- VID:PID: `051D:0003`
- Descriptor payload length: 515 bytes
- Descriptor start: `05 84 09 04 A1 01`
- Confirmed poll reports: `0x09`, `0x0C`, `0x0A`, `0x0D`, `0x0B`, `0x11`, `0x14`

A future fingerprint can be a short hash of the descriptor payload or a tuple of:

- descriptor length
- first 16 bytes
- last 16 bytes
- report-ID set

## Web UI Rules

The web UI must be capability-aware.

1. The config page exposes the selected profile and an Auto Detect option.
2. The status page should not imply support for unsupported fields.
3. Unknown/unconfirmed values should display as `Unknown`, `Not reported`, or be hidden, not shown as `0`.
4. If a field is profile-supported but currently not sampled, show `Waiting for data`.
5. USB Debug remains the field-discovery tool and should not mutate production metrics in Active Debug mode.

Examples:

- SMT2200 battery charge is confirmed; show it normally.
- SMT2200 runtime is confirmed; show it normally.
- SMT2200 input voltage is not confirmed; do not show `0 V` as if real.
- SMT2200 load percent is not confirmed; do not show `0%` as if real.

## MQTT / Home Assistant Rules

Home Assistant discovery must reflect the active/resolved profile when possible.

Recommended behavior:

1. Publish discovery only for fields that the active profile supports, or publish all fields with availability if the firmware has a robust availability model.
2. Avoid publishing numeric `0` for unsupported fields.
3. Include manufacturer/model metadata from the profile.
4. If Auto Detect is selected, update the resolved model before discovery when possible.

Current v0.3.24 limitation: MQTT discovery may run before USB auto-resolution finishes. If exact HA metadata matters immediately, users should select a concrete profile rather than Auto Detect.

## Parser Rules

1. Parser behavior dispatches by active profile.
2. Same report ID may mean different fields across profiles.
3. Scale/exponent must come from the profile mapping, not from global assumptions.
4. Status bitfields must be profile-specific.
5. Unsupported reports must be recorded and skipped, not repeatedly polled.
6. Vendor reports may be preserved as raw samples before their meaning is known.

## USB Polling Rules

Each profile owns its poll list.

- Start broad during discovery.
- Log success/STALL for each report.
- Trim production poll lists to confirmed useful reports.
- Keep debug/manual GET_REPORT capable of testing any report, including unsupported ones.

## Documentation Rules

For each model, keep a section in `docs/protocols/ups-usb-hid-protocol-notes.md` or split into per-manufacturer files once the document grows.

Each model section should include:

- Manufacturer/model.
- VID:PID.
- Descriptor capture method.
- Descriptor length/start/tail/fingerprint.
- Confirmed report table.
- Unsupported report table.
- Sample payloads.
- Parser confidence levels.
- Open questions.

## Safe Expansion Path

1. Capture descriptor with `/usb-debug`.
2. Add raw descriptor facts to protocol notes.
3. Manually test GET_REPORT Input/Feature for candidate report IDs.
4. Record sample payloads.
5. Create or update the model profile poll list.
6. Add profile-gated parser handling.
7. Update web/MQTT capability handling.
8. Build and test on hardware.
9. Only then mark fields `confirmed`.

## Current SMT2200 Profile Seed

| Field | Report | Type | Sample | Status |
|---|---:|---|---|---|
| Status bitfield | `0x09` | Input/Feature | `09 A8 4A` online | Confirmed report, bit meanings partly pending |
| Battery charge | `0x0C` | Feature | `0C 64` = 100% | Confirmed |
| Runtime | `0x0A` | Feature | `0A C0 12` = 4800s | Confirmed |
| Battery voltage-like | `0x0D` | Input/Feature | `0D B0 13` = raw 5040 | Likely, scale pending validation |
| Voltage/config-like | `0x0B` | Input/Feature | `0B 4A 15` = raw 5450 | Likely, meaning pending |
| Low charge threshold | `0x11` | Feature | `11 0A` = 10% | Confirmed |
| Beeper / audible alarm | `0x14` | Feature | `14 02` | Confirmed report, enum labels pending |
| Vendor feature | `0x92` | Feature | `92 03 00` | Preserve raw |
| Vendor feature | `0x93` | Feature | `93 01 00` | Preserve raw |
| Vendor feature | `0x94` | Feature | `94 01 00` | Preserve raw |

Open SMT2200 items:

- Exact `0x09` bit meanings need an on-battery or event-state sample.
- Direct input voltage report is not confirmed.
- Direct load percent report is not confirmed.
- Voltage-like report scaling for `0x0D` and `0x0B` needs independent validation.
