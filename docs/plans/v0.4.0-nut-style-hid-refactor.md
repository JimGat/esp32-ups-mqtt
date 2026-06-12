# v0.4.0 NUT-Style HID Refactor Plan

> **For JARVIS/Claude:** This is the line-in-the-sand architecture plan for replacing guessed APC report-ID parsing with a NUT-style HID path mapper. Do not implement ad-hoc telemetry heuristics while this refactor is active.

**Goal:** Rebuild UPS-MQTT-Bridge around the same strategy Network UPS Tools uses: parse HID descriptor paths, map logical UPS usages to metrics/status, and treat raw report IDs as transport details rather than semantic truth.

**Architecture:** The ESP32 remains GET_REPORT/poll-only for APC Smart-UPS `051D:0003`, matching NUT's strategy of disabling the interrupt pipe for this APC 5G family. A new descriptor-backed mapping layer resolves HID paths like `UPS.PowerSummary.PresentStatus.Discharging` and `UPS.Input.APCLineFailCause` into report IDs, offsets, sizes, units, and metric names. The web UI exposes confidence/provenance so Jim can tell confirmed values from pending mappings.

**Current Version Boundary:** v0.4.0-dev starts the NUT-style refactor. v0.3.x was exploratory/manual report-ID mapping.

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

### R1: Poll-Only Transport for APC 051D:0003

For SMT2200 / APC 5G family:

- Keep interrupt-IN disabled by default.
- Use GET_REPORT Feature polling only until a proven persistent interrupt state machine exists.
- No one-shot interrupt transfer allocate/submit/wait/free loops.
- No late-callback leak path.
- Transport counters must be visible.

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

Show:

- UPS Status: `OL`, `OB DISCHRG`, `UNKNOWN`, etc.
- Status Confidence: `confirmed`, `likely`, `derived`, or `unknown`
- Last Poll Age
- Transport Health: `OK`, `DEGRADED`, `ERROR`
- Active Profile: `APC Smart-UPS SMT2200 / NUT-style HID`

If status is not confirmed, show a warning:

`Status not descriptor-confirmed. Voltage readings may indicate line state, but UPS status is not mapped yet.`

#### Metrics Table

Each metric row should show:

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

### R6: Documentation Requirements

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

## Open Questions for Jim

1. Should v0.4 web UI show unconfirmed metrics by default with warning badges, or hide them unless “advanced/debug” is enabled?
2. Should MQTT publish unconfirmed/likely values, or publish only confirmed values and separate `_candidate` topics?
3. For `status`, should Home Assistant receive `UNKNOWN` until definitive mapping is found, or should it receive a derived fallback with a separate confidence field?
4. Are you willing to temporarily connect the UPS USB to a Linux box and run NUT debug output for direct comparison, or should all discovery remain on ESP32?
5. Should we add a dedicated “Power Pull Capture” web workflow before rewriting the descriptor parser?

---

## Recommended Decisions

JARVIS recommendation:

- Web UI: show likely/unconfirmed values with badges.
- MQTT: publish confirmed and likely numeric telemetry, but publish confidence topics too.
- Status: publish `UNKNOWN` unless descriptor-confirmed; publish derived line state separately as `line_state_candidate` if needed.
- Discovery: prefer one Linux/NUT debug capture if practical, because it gives us the known-good reference output.
- Claude scope: start with Phase 2 and Phase 3 only. Do not ask Claude for the full descriptor parser in the first pass.
