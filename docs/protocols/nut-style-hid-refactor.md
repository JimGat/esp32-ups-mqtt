# v0.4.0 NUT-Style HID Refactor Plan

> **For JARVIS/Claude:** This is the line-in-the-sand architecture plan for replacing guessed APC report-ID parsing with a NUT-style HID path mapper. Do not implement ad-hoc telemetry heuristics while this refactor is active.

**Goal:** Rebuild UPS-MQTT-Bridge around the same strategy Network UPS Tools uses: parse HID descriptor paths, map logical UPS usages to metrics/status, and treat raw report IDs as transport details rather than semantic truth.

**Architecture:** The ESP32 remains GET_REPORT/poll-only for APC Smart-UPS `051D:0003`, matching NUT's strategy of disabling the interrupt pipe for this APC 5G family. A new descriptor-backed mapping layer resolves HID paths like `UPS.PowerSummary.PresentStatus.Discharging` and `UPS.Input.APCLineFailCause` into report IDs, offsets, sizes, units, and metric names. The web UI exposes confidence/provenance so Jim can tell confirmed values from pending mappings.

**Current Version Boundary:** v0.4.0-dev starts the NUT-style refactor. v0.3.x was exploratory/manual report-ID mapping.

**Scope Decision:** v0.4 prioritizes trustworthy communication and confidence-gated publication over feature breadth. It is acceptable to show fewer metrics if those metrics are reliable; it is not acceptable to publish plausible-looking but unverified data.

---

## Why Strategy Changes Now

The v0.3.x manual mapping produced some useful observations but too many assumptions:

- `0x0C` battery charge appears reliable.
- `0x0B /100` likely tracks 48V battery-pack voltage.
- `0x0D /10` likely tracks AC line/input voltage.
- `0x08 /10` likely tracks load percent.
- `0x09` raw bit guesses failed pull-test validation; it stayed `09 A8 4A` while the UPS clearly transferred.
- `0x0A` runtime stayed `4800s`; it may be static/config-like or stale.

Jim's requirement is correct: status must come from the UPS' actual HID status/line-fail facts, not from voltage-derived fallback logic.

---

## NUT Comparison

### What NUT Does

From `networkupstools/nut/drivers/apc-hid.c`:

- APC `VID:PID 051d:0003` uses `disable_interrupt_pipe`.
- Status is not decoded from guessed report bits. NUT maps semantic HID paths:
  - `UPS.PowerSummary.PresentStatus.ACPresent`
  - `UPS.PowerSummary.PresentStatus.Discharging`
  - `UPS.PowerSummary.PresentStatus.Charging`
  - `UPS.PowerSummary.PresentStatus.BelowRemainingCapacityLimit`
  - `UPS.PowerSummary.PresentStatus.Overload`
  - `UPS.PowerSummary.PresentStatus.NeedReplacement`
  - `UPS.PowerSummary.PresentStatus.BatteryPresent`
- APC-specific transfer state is mapped through:
  - `UPS.Input.APCLineFailCause`
- Standard voltage paths are mapped by descriptor path:
  - `UPS.Battery.Voltage`
  - `UPS.PowerSummary.Voltage`
  - `UPS.Input.Voltage`
  - `UPS.Output.Voltage`

### What v0.3.x Does Wrong

- It treats report IDs as meaning-bearing facts.
- It guesses bit positions in `0x09` instead of deriving offsets from the descriptor.
- It has no descriptor-backed representation of usages, report type, bit offset, logical range, unit exponent, or confidence.
- The web UI does not distinguish confirmed, likely, derived, stale, and unknown fields.

---

## v0.4 Architecture Requirements

### R1: USB Communication Integrity Is Non-Negotiable

USB transport stability is the foundation for the entire project. If USB communication is not provably stable, no telemetry, status, MQTT publication, Home Assistant automation, or Web UI summary can be trusted.

For SMT2200 / APC 5G family:

- Keep interrupt-IN disabled by default, matching NUT's strategy for APC `051d:0003`.
- Use GET_REPORT Feature polling only until a proven persistent interrupt state machine exists.
- No one-shot interrupt transfer allocate/submit/wait/free loops.
- No late-callback leak path.
- No telemetry heuristics to mask transport issues.
- Any USB timeout, STALL pattern change, short read, malformed payload, or stale snapshot must be visible in logs and transport health.
- Transport counters must be visible on the Web UI's advanced/debug pages.

If transport health is degraded, the firmware must prefer missing/skipped publishes and explicit log errors over publishing questionable data.

Required counters:

- USB connected/disconnected count
- poll cycles completed
- GET_REPORT success count
- GET_REPORT timeout count
- GET_REPORT STALL count
- GET_REPORT error count
- last USB error
- last successful poll timestamp/age
- max poll duration

### R2: Descriptor-Backed HID Map

Create a model where a logical metric maps to a HID path and resolved report field:

```c
typedef enum {
    HID_REPORT_INPUT = 1,
    HID_REPORT_OUTPUT = 2,
    HID_REPORT_FEATURE = 3,
} hid_report_type_t;

typedef enum {
    MAP_CONFIRMED,
    MAP_LIKELY,
    MAP_TENTATIVE,
    MAP_UNMAPPED,
} mapping_confidence_t;

typedef struct {
    const char *nut_name;       // e.g. "battery.voltage"
    const char *hid_path;       // e.g. "UPS.Battery.Voltage"
    hid_report_type_t type;
    uint8_t report_id;
    uint16_t bit_offset;
    uint8_t bit_size;
    int8_t unit_exponent;
    float scale_override;
    mapping_confidence_t confidence;
    bool quick_poll;
} ups_hid_mapping_t;
```

Minimum v0.4 target paths:

- `UPS.PowerSummary.RemainingCapacity` -> `battery.charge`
- `UPS.Battery.Voltage` or `UPS.PowerSummary.Voltage` -> `battery.voltage`
- `UPS.Input.Voltage` -> `input.voltage`
- `UPS.PowerSummary.PresentStatus.ACPresent` -> `online/offline`
- `UPS.PowerSummary.PresentStatus.Discharging` -> `DISCHRG`
- `UPS.PowerSummary.PresentStatus.Charging` -> `CHRG`
- `UPS.PowerSummary.PresentStatus.BelowRemainingCapacityLimit` -> `LB`
- `UPS.PowerSummary.PresentStatus.Overload` -> `OVER`
- `UPS.PowerSummary.PresentStatus.NeedReplacement` -> `RB`
- `UPS.Input.APCLineFailCause` -> `input.transfer.reason`
- `UPS.PowerSummary.RunTimeToEmpty` -> `battery.runtime`, but mark as suspicious until proven live
- `UPS.PowerConverter.PercentLoad` / equivalent -> `load.percent`

### R3: No Guessed Status Bits

The firmware must not assert `OL`, `OB`, `DISCHRG`, `CHRG`, `LB`, `OVER`, or `RB` from unconfirmed bit guesses.

Allowed fallback display:

- `UNKNOWN` if no descriptor-confirmed status path exists.
- `LINE_LOW?` or `INPUT_LOW?` may appear only as a derived diagnostic, not as canonical UPS status.

Status confidence must be represented separately from the text status.

### R4: Snapshot Consistency

MQTT currently publishes while a poll cycle can still be updating metrics. v0.4 should use an atomic snapshot:

- USB poll task builds a `next_metrics` snapshot.
- At poll-cycle completion, it swaps/copies into `current_metrics` under a mutex.
- MQTT and HTTP read only complete snapshots.
- Web UI shows `snapshot_age_ms` and `last_poll_complete_ms`.

### R5: Web UI Requirements

The web UI must make trust/provenance visible.

#### Main Status Card

Keep the Web UI concepts already developed: high-level status, metrics, live logs, OTA/system controls, USB debug, and profile selection. Update the Status and Log pages so they only present UPS metrics that v0.4 can provide with high confidence.

Show on the normal Status page:

- UPS Status: `OL`, `OB DISCHRG`, `UNKNOWN`, etc.
- Status Confidence: only `confirmed` status may drive canonical status text.
- Last Poll Age
- Transport Health: `OK`, `DEGRADED`, `ERROR`
- Active Profile: `APC Smart-UPS SMT2200 / NUT-style HID`

Do not clutter the normal Status page with low-confidence data. Unconfirmed, likely, tentative, raw, and candidate values must be hidden unless Advanced Debug is enabled.

If status is not confirmed, show a clear warning:

`Status not descriptor-confirmed. UPS status will remain UNKNOWN until NUT-style PresentStatus or APCLineFailCause mapping is validated.`

#### Metrics Table

Normal mode should show only high-confidence metrics. Advanced Debug mode should add provenance columns:

- Display Name
- Value
- Unit
- Source HID Path
- Report ID / Type
- Raw Bytes
- Confidence
- Age

Example rows:

| Metric | Value | Source | Raw | Confidence |
|---|---:|---|---|---|
| Battery Charge | 64% | `UPS.PowerSummary.RemainingCapacity` | `0C 40` | confirmed |
| Battery Voltage | 45.9V | `UPS.Battery.Voltage?` | `0B EE 11` | likely |
| Input Voltage | 84.0V | `UPS.Input.Voltage?` | `0D 48 03` | likely |
| UPS Status | UNKNOWN | `PresentStatus.*` | `09 A8 4A` | unmapped |

#### HID/NUT Debug Page

Add or extend USB Debug page with:

- NUT-style mapping table
- Descriptor-resolved fields
- Poll result per mapping
- Raw report history for watched reports
- Status field truth table before/during/after pull tests

### R6: Publish Gating and Error Visibility

MQTT must not publish data unless the firmware has a high level of confidence in that data.

Rules:

- Confirmed metrics may publish normally.
- Likely/tentative/unmapped metrics must not publish to production Home Assistant topics by default.
- If a metric is skipped because confidence is too low, stale, malformed, or transport health is degraded, log an explicit missed publish / data quality message.
- Status changes must be definite. Do not publish `OL`, `OB`, `DISCHRG`, `LB`, `OVER`, or `RB` unless the backing HID path is confirmed.
- If status cannot be confirmed, publish `UNKNOWN` or skip status with a clear log message, depending on the final HA integration choice.
- Derived voltage-based line state may exist only as an Advanced Debug diagnostic, not as canonical UPS status.

### R7: Linux/NUT Collection Baseline

Jim will prepare a Linux machine for collection. The project should use Linux/NUT as the reference implementation because NUT is known to work with APC USB HID devices.

Collection goal:

- Capture NUT debug output and `upsc` values before/during/after line-power pull.
- Compare NUT `ups.status`, `battery.voltage`, `input.voltage`, `input.transfer.reason`, and any APC-specific fields with ESP32 raw reports.
- Use NUT-confirmed HID paths to promote ESP32 mappings from likely/tentative to confirmed.

### R8: Documentation Requirements

Update:

- `docs/protocols/ups-usb-hid-protocol-notes.md`
- `docs/protocols/ups-profile-table-design.md`
- new `docs/protocols/nut-style-hid-refactor.md`
- web UI help text

Docs must explicitly say:

- v0.3.x manual report IDs are exploratory.
- v0.4.x uses NUT-style HID path mapping.
- `0x09` raw status guesses are deprecated.
- Pull-test validation is required before marking status confirmed.

---

## Implementation Phases

### Phase 1: Establish v0.4 Baseline

- Bump version to `v0.4.0-dev` in all required places.
- Add this plan.
- Do not rewrite parser yet.
- Commit as the strategy boundary.

### Phase 2: Transport Counters and Snapshot Locking

Files:

- `main/usb_host_manager.c`
- `main/usb_host_manager.h`
- `main/apc_hid_parser.c`
- `main/apc_hid_parser.h`
- `main/http_server.c`

Tasks:

1. Add `usb_transport_stats_t`.
2. Increment counters in GET_REPORT path.
3. Expose `/api/transport` JSON endpoint.
4. Add metric snapshot mutex/copy API.
5. Make MQTT read atomic snapshots only.

Verification:

- Run 30 minutes with UPS connected.
- No leaked transfers.
- Poll counters increase predictably.
- MQTT values correspond to complete poll snapshots.

### Phase 3: Static NUT-Style Mapping Table

Files:

- Create `main/ups_hid_map.h`
- Create `main/ups_hid_map.c`
- Modify `main/CMakeLists.txt`
- Modify `main/apc_hid_parser.c`

Tasks:

1. Define mapping structs and confidence enum.
2. Add static SMT2200 mapping table using current known report IDs, but mark uncertain paths as `likely` or `unmapped`.
3. Preserve raw report bytes per mapping.
4. Decode values through mapping table rather than switch/case semantics where possible.

Verification:

- Existing values still decode: charge, battery voltage, input voltage, load.
- Status remains `UNKNOWN` unless descriptor-confirmed.
- Web UI shows source/confidence.

### Phase 4: Descriptor Field Resolver

Goal: Move from static guesses to descriptor-resolved report offsets.

Tasks:

1. Preserve complete 515-byte SMT2200 descriptor sample in docs/reference or generated C fixture.
2. Implement minimal HID descriptor parser for Report ID, report type, usage path, offset, size, logical range, unit exponent.
3. Resolve NUT target paths from descriptor.
4. Generate/log a field table on boot.

Verification:

- Boot log prints fields for `PresentStatus.*` and voltage paths.
- Field offsets match raw report lengths.
- No status flags assigned until field paths are resolved.

### Phase 5: Status Discovery and Pull-Test Harness

Tasks:

1. Add watched status fields endpoint.
2. Add a web UI “Power Pull Capture” mode:
   - capture before/during/after raw reports
   - show changed fields
   - export JSON/log text
3. During test, compare to UPS display.
4. Mark fields confirmed only after observed state transitions.

Acceptance Criteria:

- Pulling line power changes an explicit descriptor-resolved field:
  - `ACPresent`, `Discharging`, or `APCLineFailCause`.
- Web UI reports `OB DISCHRG` only from that confirmed field.
- Plugging back in reports `OL` from confirmed field.

### Phase 6: Claude Rewrite Execution

Use Claude Code only after this plan is committed.

Claude prompt must include:

- Repo path: `/home/dev/projects/esp32-ups-mqtt`
- Branch: `dev`
- Version boundary: `v0.4.0-dev`
- Do not push `main`
- Do not add telemetry heuristics
- Follow NUT-style HID path mapping
- First deliverable: transport counters + snapshot locking + static mapping table skeleton
- Required build: `. /home/dev/esp/esp-idf/export.sh && idf.py build`
- Require summary of changed files and test/build output

Parent/JARVIS verification after Claude:

- inspect `git diff`
- run build
- verify version files
- verify no secret/config exposure
- manually review status mapping for guessed bits

---

## Jim's Design Decisions

These are accepted requirements for the v0.4 refactor:

1. Web UI visibility:
   - Hide unconfirmed/likely/tentative metrics unless Advanced Debug is enabled.
   - Maintain the existing Web UI concepts: Status page, logs, OTA/system controls, USB debug, and profile/config flows.
   - Update Status and Log pages so UPS metrics match only what the firmware can provide confidently.

2. MQTT publication:
   - Do not publish a metric unless there is a high level of confidence in the data.
   - If a value is skipped because of confidence, stale data, malformed payload, or transport issue, show an error or missed-publish log entry.

3. Status:
   - Status changes must be definite.
   - Do not publish or display canonical OB/OL/DISCHRG/LB/etc. from guessed bits or derived voltage fallback.
   - If definitive status is not mapped yet, status must remain UNKNOWN or be skipped with explicit logging.

4. Linux/NUT reference:
   - Jim will prepare a Linux machine for collection.
   - Use NUT as the reference behavior because NUT is known to work on Linux.
   - ESP32 mapping confidence should be promoted only after comparison with NUT output and/or descriptor-confirmed fields.

5. Claude implementation:
   - Claude Code is likely the right worker for the rewrite.
   - Do not start Claude with an open-ended rewrite prompt.
   - First Claude scope should be bounded to transport counters, atomic snapshots, confidence gating, Web UI adjustments, and static NUT-style mapping scaffolding.

## v0.4.1-dev scaffold implementation note

The first implementation pass adds a static NUT-style mapping/provenance scaffold (`main/ups_hid_map.*`) and transport health counters. It does not implement a dynamic HID descriptor parser yet.

Important runtime behavior:

- SMT2200 report `0x09` is retained only as an unmapped raw status candidate. It no longer drives canonical `OL`, `OB`, `CHRG`, or `DISCHRG` because pull-test evidence showed it remained unchanged during transfer.
- Canonical UPS status remains `UNKNOWN` unless a confirmed status path sets `status_confidence=confirmed`.
- MQTT publishes `UNKNOWN` for status when status confidence is not confirmed and logs a `DATA_QUALITY` warning instead of publishing guessed state.
- HTTP `/status` and `/metrics` read atomic metric snapshots and expose active profile, status confidence, transport health, and last poll age.
- Linux/NUT baseline capture is still required before promoting SMT2200 status paths from unmapped/likely to confirmed.

## v0.4.2-dev descriptor detection pass

This pass adds a small host-tested HID report descriptor parser (`main/hid_descriptor_parser.*`) following the NUT strategy of treating report IDs as transport details and resolving semantic HID usage paths first. The parser tracks Usage Page, Usage, Collection path, Report ID, Report Type, Report Size/Count, bit offsets, logical ranges, and unit exponent.

When a HID report descriptor is dumped through USB Debug mode, firmware now parses the payload-only descriptor and logs NUT-relevant fields such as `UPS.PowerSummary.PresentStatus.*`, `UPS.PowerSummary.RemainingCapacity`, `UPS.Input.Voltage`, and `UPS.Battery.Voltage` with report ID/bit offset/size provenance. It also records concise `field ...` events in the USB debug ring.

This is detection/provenance only: SMT2200 canonical status still remains `UNKNOWN` until a descriptor-resolved status field is validated against Linux/NUT and/or a controlled pull test.

## v0.4.3-dev USB debug descriptor UX fix

The HID descriptor dump button now switches the runtime USB debug mode to Active Debug before queuing the descriptor command. The previous `v0.4.2-dev` UI let the browser queue a descriptor dump while the firmware was still in Normal Bridge mode if the user changed the dropdown but did not click `Apply Debug Mode` first. That produced `descriptor queued` followed by `debug inactive` and no descriptor transfer.

## v0.4.4-dev USB debug record visibility fix

The captured-records endpoints now return the latest 64 records by default instead of the oldest 16 records. Descriptor dumps create many chunk and field records; the previous endpoint could show only early boot/config lines and make a successful descriptor dump look like it disappeared. The USB debug ring was also increased to 128 records so a full descriptor dump plus NUT field events remain visible long enough to copy them.

## v0.4.5-dev descriptor dump stability fix

The v0.4.4 debug-record expansion was rolled back to the small fixed stack-buffer handlers because field testing showed the web UI could hang after clicking `Dump HID Report Descriptor`. The firmware now keeps the original 64-record ring and 16-record HTTP page size, but returns the latest records by default and clears the debug ring just before queuing a descriptor dump. This preserves the low-memory behavior of v0.4.3 while making the post-dump records window useful.

## v0.4.6-dev descriptor POST stability fix

The descriptor dump HTTP handler no longer clears debug records or resets the debug command queue from the HTTP server context before enqueueing the descriptor command. Field testing on v0.4.5 showed the browser could sit forever on `Loading...` and the debug ring would contain only the pre-existing `interface claimed` record, indicating the POST did not complete the queue path. The handler now keeps the v0.4.3-style low-memory behavior, queues the descriptor command, and returns a plain text 200 response instead of a redirect.

## SMT2200 report 0x05 probe evidence

Field test after the 515-byte descriptor capture: safe manual `GET_REPORT` for report `0x05` returned the same two-byte payload for both Feature (`type=3`) and Input (`type=1`): `05 04`. This proves report `0x05` is readable, but the descriptor context around report `0x05` is transfer/config-style one-bit fields rather than a clean canonical `PresentStatus` source. Do not promote `0x05` directly to `OL/OB`; keep it as descriptor-backed evidence to compare against NUT/Linux and controlled line-state tests.

## SMT2200 baseline descriptor-backed probe values

Online/on-mains baseline probes while in Active Debug:

- `GET_REPORT type=3 report=0x05 len=8 safe` returned `05 04`; `GET_REPORT type=1 report=0x05 len=8 safe` also returned `05 04`.
- `GET_REPORT type=3 report=0x12 len=8 safe` returned `12 FF FF`. The descriptor labels report `0x12` usage `UPS.PowerSummary.PresentStatus.Charging` as a 16-bit Feature with logical range -1..32767, so `0xFFFF` likely represents signed -1 / unknown / not-applicable rather than a true boolean charging state.
- `GET_REPORT type=3 report=0x14 len=8 safe` returned `14 02`. Existing normal polling also returns `14 02`; descriptor context includes `NeedReplacement` and audible-alarm/beeper semantics, so this remains evidence only.

Do not promote these baseline values to canonical `OL/OB/CHRG/RB` until compared against Linux/NUT or a controlled line-state/battery-state change.

## SMT2200 controlled AC-pull probe comparison

Controlled line-state test comparing on-mains vs AC input pulled while in Active Debug:

| Report | Type | On mains | AC pulled | Changed? |
|---|---:|---|---|---|
| `0x05` | Feature `3` | `05 04` | `05 04` | no |
| `0x05` | Input `1` | `05 04` | `05 04` | no |
| `0x12` | Feature `3` | `12 FF FF` | `12 FF FF` | no |
| `0x12` | Input `1` | `12 FF FF` | `12 FF FF` | no |
| `0x14` | Feature `3` | `14 02` | `14 02` | no |
| `0x14` | Input `1` | `14 02` | `14 02` | no |

Result: reports `0x05`, `0x12`, and `0x14` did not respond to AC line-state changes and must not be used to infer canonical `OL`/`OB`. Continue to treat report `0x09` as raw/unmapped and status as `UNKNOWN` until a changing descriptor-backed line-state source is identified. The already-confirmed useful AC-pull signals remain measured telemetry like input voltage (`0x0D`) rather than canonical UPS status.

## New direction: dynamic NUT-style runtime HID map

Do not make the APC SMT2200 model table the source of truth for status or telemetry semantics. Firmware revisions may legitimately change report IDs, bit packing, report type, scale, or unit metadata. The bridge should instead reproduce the important NUT/usbhid-ups discovery pattern at runtime:

1. Parse the connected UPS HID report descriptor.
2. Build a runtime semantic map from HID usage paths to UPS/NUT concepts.
3. Preserve report type, report ID, bit offset, bit size, logical range, unit, unit exponent, raw bytes, decoded value, age, confidence, and source for every mapped field.
4. Poll the descriptor-derived fields safely using GET_REPORT where appropriate.
5. Compose canonical `ups.status` only from confirmed semantic sources like `UPS.PowerSummary.PresentStatus.ACPresent`, `Discharging`, `Charging`, `BelowRemainingCapacityLimit`, `NeedReplacement`, and `Overload`.
6. Keep model/profile tables only for quirks: preferred report type, broken descriptor overrides, ignored fields, safe lengths, scale fixes, and confidence policy.

The target architecture is therefore:

- Generic HID descriptor parser: report layouts and usage paths.
- NUT semantic resolver: usage paths to `battery.charge`, `input.voltage`, `ups.status.*`, etc.
- Runtime poll plan: reads the required report IDs/types and extracts fields dynamically.
- Quirk table: model/firmware exceptions, never the default source of truth.
- Publisher/UI: exposes provenance and publishes canonical values only when confidence is high enough; otherwise publish `UNKNOWN` or diagnostic/derived candidates separately.

This replaces raw report-ID guessing. Reports like `0x05`, `0x09`, `0x12`, and `0x14` are evidence only until tied to descriptor-resolved semantic paths and validated by behavior or NUT/Linux output.

## v0.4.7-dev runtime NUT map scaffold

Started implementation of the new dynamic direction with a host-tested `nut_runtime_map` module. It converts descriptor-resolved HID fields into semantic NUT runtime map entries such as `battery.charge`, `input.voltage`, and `ups.status.acpresent`, preserving report type, report ID, bit offset, bit size, logical range, unit exponent, source, and confidence. It also includes a conservative status composer that returns `UNKNOWN` unless descriptor-confirmed status sources have live values. The descriptor dump callback now builds and logs a runtime semantic map from parsed descriptor fields; this is evidence/provenance only and does not yet change MQTT status publishing.

## v0.4.14-dev: Strict Descriptor-First Discovery Mode

Per user directive: 'We can\'t use normal polling! ... All I want this code to do at this point is output a proper HID Descriptor Query.'

This build enforces a strict state machine:
1. USB callback detects device, claims interface, and sets `descriptor_needed = true`. ZERO blocking work.
2. USB task calls `usb_host_client_handle_events`, then checks the flag. If true, it safely submits the GET_DESCRIPTOR transfer from task context.
3. Transfer callback receives the descriptor, hex-dumps it to serial, and sets `descriptor_complete = true`.
4. **Telemetry polling is entirely disabled/commented out.** The USB task will idle until the descriptor is complete.

This eliminates all ESP-IDF USB context violations and provides a clean, isolated HID Report Descriptor hex dump for NUT-style mapping analysis.

## v0.4.16-dev: strict startup discovery cleanup

This build removes remaining startup noise and legacy paths from descriptor discovery:
- MQTT is not initialized.
- Telemetry and power-event tasks are not started.
- HTTP scanner 404 noise is suppressed at the ESP log tag level.
- USB task runs at higher priority with larger stack.
- USB task performs only USB library events, USB client events, automatic HID report descriptor request, transfer callback servicing, and heartbeat logging.
- Legacy GET_REPORT telemetry polling is not reachable in this build, even after descriptor completion.

## v0.4.17-dev: cooperative USB discovery scheduling

v0.4.16 made the USB discovery task too aggressive and higher-priority than appropriate for Wi-Fi/LwIP/httpd on ESP32. Symptoms were severe ping loss and intermittent web responsiveness while waiting for USB attach. v0.4.17 keeps descriptor-first/no-polling behavior but restores cooperative scheduling:
- USB task priority lowered from 10 to 4.
- USB event waits reduced to short 5 ms checks.
- Loop delay increased to 100 ms.
- MQTT remains disabled; broker is logged only as configured, not active.

## v0.4.18-dev: network-only diagnostic build

Purpose: isolate severe ping loss / intermittent web responsiveness seen in v0.4.16-v0.4.17 while waiting for USB attach.

This build intentionally disables USB host initialization and the USB task entirely while keeping Wi-Fi, HTTP, OTA, SNTP, and strict no-MQTT behavior. It does not attempt descriptor discovery. Expected use:
- If ping/web become stable, root cause is in ESP-IDF USB host install/event handling or its interaction with Wi-Fi on this board/build.
- If ping/web remain unstable, root cause is outside the USB host/descriptor path and should be investigated in Wi-Fi/HTTP/runtime startup.

## v0.4.19-dev: USB host install-only diagnostic

v0.4.18 proved Wi-Fi/HTTP is solid when the USB host path is disabled. This build isolates the next boundary:
- Wi-Fi/HTTP/OTA/SNTP remain enabled.
- MQTT remains disabled.
- `usb_host_install()` is called with the normal host config.
- No USB client is registered.
- No USB task is created.
- No descriptor request or telemetry polling is possible.

Interpretation:
- If ping/web break here, the root cause is USB host install/PHY/interrupt side effects.
- If ping/web remain solid here, the root cause is in USB client registration, event handling, or the USB task loop.

## v0.4.20-dev: USB client registration-only diagnostic

v0.4.19 proved Wi-Fi/HTTP remains mostly solid with `usb_host_install()` only. This build isolates client registration:
- Wi-Fi/HTTP/OTA/SNTP remain enabled.
- MQTT remains disabled.
- `usb_host_install()` is called.
- `usb_host_client_register()` is called with the normal async callback config.
- No USB task is created.
- `usb_host_lib_handle_events()` and `usb_host_client_handle_events()` are never called.
- No descriptor request or telemetry polling is possible.

Interpretation:
- If ping/web break here, the root cause is client registration/callback setup.
- If ping/web remain solid here, the root cause is in the USB event handling loop/task or callback work during attach.

## v0.4.21-dev: USB library-events-only diagnostic

v0.4.20 proved Wi-Fi/HTTP remains solid with USB host install plus async client registration when no event loop runs. This build isolates library event pumping:
- Wi-Fi/HTTP/OTA/SNTP remain enabled.
- MQTT remains disabled.
- USB host is installed and client is registered.
- A USB task is created, but it only calls `usb_host_lib_handle_events()`.
- It never calls `usb_host_client_handle_events()`, so the callback cannot run.
- No device open/claim, descriptor request, or telemetry polling is possible.

Interpretation:
- If ping/web break here, the root cause is the library event pump/task scheduling.
- If ping/web remain solid here, the root cause is specifically in client event handling or callback work during attach.

## v0.4.22-dev: USB client-events observe-only diagnostic

v0.4.21 proved Wi-Fi/HTTP remains solid with USB host install, client registration, and `usb_host_lib_handle_events()` only. This build enables the next boundary:
- Wi-Fi/HTTP/OTA/SNTP remain enabled.
- MQTT remains disabled.
- USB host is installed and client is registered.
- A USB task pumps both `usb_host_lib_handle_events()` and `usb_host_client_handle_events()`.
- The USB callback is observe-only: it logs NEW_DEV/DEV_GONE and immediately returns.
- It does not open the device, claim interface, request descriptor, or poll reports.

Interpretation:
- If ping/web break here, the root cause is client event dispatch/callback invocation itself.
- If ping/web remain solid and NEW_DEV is observed, the root cause is in the callback's device open/descriptor path.

## v0.4.23-dev: USB device descriptor-only diagnostic

v0.4.22 proved Wi-Fi/HTTP remains solid with both USB lib and client event pumps running while the callback only observes events. This build advances one boundary:
- Wi-Fi/HTTP/OTA/SNTP remain enabled.
- MQTT remains disabled.
- On NEW_DEV, callback opens the device, reads the standard USB device descriptor, logs VID/PID/class/config count, then closes immediately.
- It does not claim HID interface, read config descriptor, request HID report descriptor, or poll reports.

Interpretation:
- If ping/web break here, root cause is device open/get-device-descriptor/close path.
- If ping/web remain solid and VID/PID logs, root cause is later: active config descriptor, interface claim, or HID report descriptor request.

## v0.4.24-dev: USB active config descriptor-only diagnostic

v0.4.23 proved Wi-Fi/HTTP remains solid while opening the UPS, reading the USB device descriptor, logging VID/PID, and closing. This build advances one boundary:
- On NEW_DEV, callback opens the device, reads the standard device descriptor, reads the active config descriptor, logs interface summary, then closes.
- It does not claim HID interface, request HID report descriptor, or poll reports.

Interpretation:
- If ping/web break here, root cause is active config descriptor read or descriptor parsing/logging.
- If ping/web remain solid and config/interface logs appear, root cause is later: interface claim or HID report descriptor request.

## v0.4.25-dev: config descriptor diagnostic with explicit attach visibility

v0.4.24 remained network-stable but the provided log did not show a NEW_DEV event, so it did not exercise the active config descriptor path. This build keeps the same diagnostic boundary and improves observability:
- NEW_DEV and DEV_GONE counters are logged.
- NEW_DEV is logged as a warning-level line so it is hard to miss.
- Heartbeat cadence is reduced to 10 seconds and includes `new_dev` / `dev_gone` counters.
- If no NEW_DEV is seen, heartbeat explicitly logs that it is still waiting for the USB host client attach event.

Interpretation remains the same as v0.4.24 once NEW_DEV appears.

## v0.4.26-dev: USB HID interface claim-only diagnostic

v0.4.25 proved Wi-Fi/HTTP remains solid through NEW_DEV, device open, USB device descriptor read, active config descriptor read, interface descriptor parsing, and device close. This build advances one boundary:
- On NEW_DEV, callback opens the device, reads device/config descriptors, claims HID interface 0 alt 0, releases it, then closes.
- It does not request the HID report descriptor or poll reports.

Interpretation:
- If ping/web break here, root cause is HID interface claim/release.
- If ping/web remain solid and claim/release logs ESP_OK, root cause is later: HID report descriptor control transfer submission/callback.
