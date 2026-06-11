/*
 * ═══════════════════════════════════════════════════════════════════════════
 * APC HID PARSER - Architecture & Protocol Notes
 * ═══════════════════════════════════════════════════════════════════════════
 * PURPOSE:
 * Decodes raw USB HID reports from APC UPS devices into a unified `ups_metrics_t`
 * struct for MQTT publishing.
 *
 * REPORT ID MAPPING (Back-UPS vs Smart-UPS):
 * - 0x0C: Battery Charge & Runtime (Works on both)
 * - 0x06: Status Flags (Online, Charging, Discharging) (Works on both)
 * - 0x0D: Battery Voltage (Works on both)
 * - 0x31: Input Voltage (⚠️ HARDCODED FOR BACK-UPS. STALLS ON SMT2200)
 * - 0x50: Load Percent    (⚠️ HARDCODED FOR BACK-UPS. STALLS ON SMT2200)
 *
 * PENDING TASK (SMT2200 Support):
 * The Smart-UPS SMT2200 (VID:PID 051D:0003) uses different Report IDs for
 * Input Voltage and Load. Hardcoded requests for 0x31/0x50 will STALL.
 * Solution: Extract the HID Report Descriptor via Linux `usbhid-dump -m 051d`,
 * decode the dynamic Usage Page/Usage ID mappings, and update this parser
 * to route requests based on the detected UPS model.
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include "apc_hid_parser.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "apc_hid_parser";
static ups_metrics_t current_metrics = {0};
static ups_profile_t configured_profile = UPS_PROFILE_AUTO;
static ups_profile_t active_profile = UPS_PROFILE_APC_GENERIC_HID;

static bool is_smt2200_profile(void)
{
    return active_profile == UPS_PROFILE_APC_SMT2200;
}

void apc_hid_parser_init(ups_profile_t profile)
{
    configured_profile = ups_profile_validate((uint8_t)profile);
    active_profile = (configured_profile == UPS_PROFILE_AUTO) ? UPS_PROFILE_APC_GENERIC_HID : configured_profile;
    memset(&current_metrics, 0, sizeof(ups_metrics_t));
    current_metrics.valid = false;

    // Set default values
    strcpy(current_metrics.driver_name, "esp32-usb-hid");
    strcpy(current_metrics.driver_version, "1.0.0");
    strcpy(current_metrics.driver_state, "running");
    strcpy(current_metrics.battery_type, "PbAc");
    strcpy(current_metrics.power_failure_status, "OK");

    ESP_LOGI(TAG, "🔋 APC HID parser initialized (configured=%s, active=%s)",
             ups_profile_name(configured_profile), ups_profile_name(active_profile));
}

void apc_hid_parser_set_profile(ups_profile_t profile)
{
    active_profile = ups_profile_validate((uint8_t)profile);
    if (active_profile == UPS_PROFILE_AUTO) {
        active_profile = UPS_PROFILE_APC_GENERIC_HID;
    }
    strlcpy(current_metrics.driver_name, ups_profile_name(active_profile), sizeof(current_metrics.driver_name));
    ESP_LOGI(TAG, "🔋 APC HID parser active profile: %s", ups_profile_name(active_profile));
}

ups_profile_t apc_hid_parser_get_profile(void)
{
    return active_profile;
}

// Helper function to print hex dump
static void log_hex_dump(const char *prefix, const uint8_t *data, size_t length)
{
    char hex_str[256];
    char ascii_str[64];
    int hex_pos = 0;
    int ascii_pos = 0;

    for (size_t i = 0; i < length && hex_pos < sizeof(hex_str) - 4; i++) {
        hex_pos += snprintf(hex_str + hex_pos, sizeof(hex_str) - hex_pos, "%02X ", data[i]);
        ascii_str[ascii_pos++] = (data[i] >= 32 && data[i] <= 126) ? data[i] : '.';
    }
    ascii_str[ascii_pos] = '\0';

    ESP_LOGI(TAG, "%s [%d bytes]: %s | %s", prefix, length, hex_str, ascii_str);
}

bool apc_hid_parse_report(uint8_t report_id, const uint8_t *data, size_t length, ups_metrics_t *metrics)
{
    if (data == NULL || length == 0) {
        return false;
    }

    // Log raw HID report
    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    ESP_LOGI(TAG, "📦 RAW HID REPORT");
    ESP_LOGI(TAG, "   Report ID: 0x%02X (%d)", report_id, report_id);
    log_hex_dump("   Data", data, length);

    // Use current_metrics if metrics pointer is NULL
    ups_metrics_t *target = (metrics != NULL) ? metrics : &current_metrics;
    bool updated = false;

    ESP_LOGI(TAG, "🔍 PARSING LOGIC:");
    switch (report_id) {
        case 0x0C:  // Battery charge and runtime (UPS.PowerSummary)
            ESP_LOGI(TAG, "   Type: Battery Charge & Runtime (UPS.PowerSummary)");
            // Some UPS models send charge as a single byte (0x64=100%)
            // Others include runtime as bytes 2-3
            if (length >= 2) {
                target->battery_charge = (float)data[1];
                ESP_LOGI(TAG, "   ├─ Byte[1]: Battery charge = %d%%", data[1]);

                if (length >= 4) {
                    uint16_t runtime_seconds = data[2] | (data[3] << 8);
                    target->battery_runtime = (float)runtime_seconds;
                    ESP_LOGI(TAG, "   ├─ Byte[2-3]: Runtime = %d seconds (%.1f min)", runtime_seconds, runtime_seconds / 60.0f);
                } else {
                    ESP_LOGI(TAG, "   ├─ Runtime: Not in this report (need 4+ bytes, got %d)", length);
                }

                ESP_LOGI(TAG, "   └─ Result: Battery %.0f%%, Runtime %.0fs",
                         target->battery_charge, target->battery_runtime);
                updated = true;
            }
            break;

        case 0x06:  // Status flags
            ESP_LOGI(TAG, "   Type: Status Flags");
            if (length >= 3) {
                /* APC status flags layout (from NUT hid-ups.c):
                   Byte 1: ACPresent(0x01), Charging(0x02), Discharging(0x04),
                           LowBattery(0x08), ShutdownImminent(0x10)
                   Byte 2: Overload(0x01), BatteryBad(0x02), ReplaceBattery(0x04) */
                uint8_t status_byte = data[1];
                target->status.online = (status_byte & 0x01) != 0;      // ACPresent
                target->status.discharging = (status_byte & 0x04) != 0;  // Discharging
                target->status.charging = (status_byte & 0x02) != 0;     // Charging
                target->status.low_battery = (status_byte & 0x08) != 0;  // LowBattery

                ESP_LOGI(TAG, "   └─ Status byte 0x%02X: ACPresent=%d Charging=%d Discharging=%d LowBatt=%d",
                         status_byte, target->status.online, target->status.charging,
                         target->status.discharging, target->status.low_battery);
                updated = true;
            }
            break;

        case 0x08:  // Battery nominal voltage (UPS.PowerSummary.ConfigVoltage)
            ESP_LOGI(TAG, "   Type: Battery Nominal Voltage");
            if (length >= 3) {
                // 16-bit value with Exponent = -2, so divide by 100
                // Raw data example: 08 B0 04 = 0x04B0 = 1200 / 100 = 12V
                uint16_t voltage_raw = data[1] | (data[2] << 8);
                target->battery_nominal_voltage = (float)voltage_raw / 100.0f;
                ESP_LOGI(TAG, "   └─ Raw: 0x%04X → %.1fV", voltage_raw, target->battery_nominal_voltage);
                updated = true;
            }
            break;

        case 0x09:
            if (is_smt2200_profile()) {
                ESP_LOGI(TAG, "   Type: SMT2200 PresentStatus bitfield");
                if (length >= 3) {
                    uint16_t flags = data[1] | (data[2] << 8);
                    // Confirmed online/charging sample for Jim's SMT2200: 09 A8 4A = 0x4AA8.
                    // Real-world state at capture: online, charging, 14% load, no overload,
                    // no replace-battery alarm. Therefore bit 3 is OL and bit 5 is CHRG.
                    // Bits 7 and 11 are explicitly NOT overload/replace-battery for this sample.
                    target->status.online = (flags & (1u << 3)) != 0;
                    target->status.charging = (flags & (1u << 5)) != 0;
                    target->status.discharging = !target->status.online;
                    target->status.low_battery = false;
                    target->status.overload = false;
                    target->status.replace_battery = false;
                    target->status.boost = false;
                    target->status.trim = false;
                    strlcpy(target->power_failure_status, target->status.online ? "OK" : "ON_BATTERY", sizeof(target->power_failure_status));
                    ESP_LOGI(TAG, "   └─ SMT2200 raw status flags: 0x%04X online=%d charging=%d (alarm bits unassigned)",
                             flags, target->status.online, target->status.charging);
                    updated = true;
                }
            } else {
                // Generic/Back-UPS: Report 0x09 is an unreliable voltage fallback.
                ESP_LOGI(TAG, "   Type: Battery Voltage (Report 0x09 - generic fallback)");
                if (length >= 3 && target->battery_voltage < 1.0f) {
                    uint16_t voltage_raw = data[1] | (data[2] << 8);
                    target->battery_voltage = (float)voltage_raw / 100.0f;
                    ESP_LOGI(TAG, "   └─ Fallback voltage: %.2fV (0x%04X / 100)", target->battery_voltage, voltage_raw);
                    updated = true;
                } else {
                    ESP_LOGI(TAG, "   └─ Skipped (0x0D already provided %.2fV)", target->battery_voltage);
                }
            }
            break;

        case 0x0A:  // Battery runtime (UPS.PowerSummary.RunTimeToEmpty) - Feature Report
            ESP_LOGI(TAG, "   Type: Battery Runtime");
            if (length >= 3) {
                uint16_t runtime_raw = data[1] | (data[2] << 8);
                target->battery_runtime = (float)runtime_raw;
                ESP_LOGI(TAG, "   └─ Runtime: %ds (%.1f min) [0x%04X]",
                         runtime_raw, runtime_raw / 60.0f, runtime_raw);
                updated = true;
            }
            break;

        case 0x0B:  // Voltage/config-like value
            if (is_smt2200_profile()) {
                ESP_LOGI(TAG, "   Type: SMT2200 Voltage-like Report 0x0B");
                if (length >= 3) {
                    uint16_t raw = data[1] | (data[2] << 8);
                    target->battery_nominal_voltage = (float)raw / 100.0f;  // scale pending, confirmed sample 0x154A=5450
                    ESP_LOGI(TAG, "   └─ Raw: 0x%04X → %.2fV (scale/meaning pending)", raw, target->battery_nominal_voltage);
                    updated = true;
                }
            } else {
                ESP_LOGI(TAG, "   Type: Battery Nominal Voltage");
                if (length >= 2) {
                    target->battery_nominal_voltage = (float)data[1];
                    ESP_LOGI(TAG, "   └─ Nominal: %.0fV", target->battery_nominal_voltage);
                    updated = true;
                }
            }
            break;

        case 0x0D:  // Battery voltage (interrupt endpoint) - 16-bit LE, exponent -2
            // Confirmed on APC Back-UPS XS 1000M (4x12V = 48V pack):
            //   0D B0 13 → 0x13B0 = 5040 / 100 = 50.40V (charging)
            //   0D 64 14 → 0x1464 = 5220 / 100 = 52.20V (float)
            ESP_LOGI(TAG, "   Type: Battery Voltage (interrupt)");
            if (length >= 3) {
                uint16_t voltage_raw = data[1] | (data[2] << 8);
                target->battery_voltage = (float)voltage_raw / 100.0f;
                ESP_LOGI(TAG, "   └─ Battery: %.2fV (0x%04X / 100)", target->battery_voltage, voltage_raw);
                updated = true;
            }
            break;

        case 0x0E:  // Full charge capacity (NOT low battery threshold!)
            ESP_LOGI(TAG, "   Type: Full Charge Capacity");
            if (length >= 2) {
                // This is FullChargeCapacity = 100%, not the low battery threshold
                // Don't store this as low_battery_charge_threshold
                ESP_LOGI(TAG, "   └─ Full Capacity: %.0f%% (not low threshold)", (float)data[1]);
                // Do NOT set updated = true, we don't want to store this
            }
            break;

        case 0x0F:  // Battery warning threshold
            ESP_LOGI(TAG, "   Type: Battery Warning Threshold");
            if (length >= 2) {
                target->battery_warning_threshold = (float)data[1];
                ESP_LOGI(TAG, "   └─ Threshold: %.0f%%", target->battery_warning_threshold);
                updated = true;
            }
            break;

        case 0x10:  // Beeper status
            ESP_LOGI(TAG, "   Type: Beeper Status");
            if (length >= 2) {
                const char *beeper_state[] = {"disabled", "enabled", "muted"};
                uint8_t beeper_val = data[1];
                if (beeper_val < 3) {
                    strncpy(target->beeper_status, beeper_state[beeper_val], sizeof(target->beeper_status) - 1);
                    ESP_LOGI(TAG, "   └─ Beeper: %s", target->beeper_status);
                    updated = true;
                }
            }
            break;

        case 0x11:  // Battery low charge threshold (UPS.PowerSummary.RemainingCapacityLimit)
            ESP_LOGI(TAG, "   Type: Battery Low Charge Threshold");
            if (length >= 2) {
                target->low_battery_charge_threshold = (float)data[1];
                ESP_LOGI(TAG, "   └─ Threshold: %.0f%%", target->low_battery_charge_threshold);
                updated = true;
            }
            break;

        case 0x12:  // Low battery runtime threshold
            ESP_LOGI(TAG, "   Type: Low Battery Runtime Threshold");
            if (length >= 3) {
                uint16_t runtime = data[1] | (data[2] << 8);
                target->low_battery_runtime_threshold = (float)runtime;
                ESP_LOGI(TAG, "   └─ Threshold: %.0fs", target->low_battery_runtime_threshold);
                updated = true;
            }
            break;

        case 0x15:  // Shutdown timer
            ESP_LOGI(TAG, "   Type: Shutdown Timer");
            if (length >= 3) {
                int16_t timer = (int16_t)(data[1] | (data[2] << 8));
                target->shutdown_timer = (float)timer;
                ESP_LOGI(TAG, "   └─ Timer: %.0fs", target->shutdown_timer);
                updated = true;
            }
            break;

        case 0x16:  // Status bits (PresentStatus)
            ESP_LOGI(TAG, "   Type: Present Status Bits");
            if (length >= 2) {
                uint8_t present_status = data[1];
                target->status.online = (present_status & 0x01) != 0;
                target->status.discharging = (present_status & 0x02) != 0;
                target->status.charging = (present_status & 0x04) != 0;
                target->status.low_battery = (present_status & 0x08) != 0;
                target->status.overload = (present_status & 0x10) != 0;
                target->status.replace_battery = (present_status & 0x20) != 0;
                target->status.boost = (present_status & 0x40) != 0;
                target->status.trim = (present_status & 0x80) != 0;

                ESP_LOGI(TAG, "   └─ Status: 0x%02X [%s%s%s%s%s%s%s%s]", present_status,
                         target->status.online ? "OL " : "",
                         target->status.discharging ? "DISCHRG " : "",
                         target->status.charging ? "CHRG " : "",
                         target->status.low_battery ? "LB " : "",
                         target->status.overload ? "OVER " : "",
                         target->status.replace_battery ? "RB " : "",
                         target->status.boost ? "BOOST " : "",
                         target->status.trim ? "TRIM" : "");
                updated = true;
            }
            break;

        case 0x17:  // Reboot timer
            ESP_LOGI(TAG, "   Type: Reboot Timer");
            if (length >= 3) {
                uint16_t timer = data[1] | (data[2] << 8);
                target->reboot_timer = (float)timer;
                ESP_LOGI(TAG, "   └─ Timer: %.0fs", target->reboot_timer);
                updated = true;
            }
            break;

        case 0x18:  // Self-test result
            ESP_LOGI(TAG, "   Type: Self-Test Result");
            if (length >= 2) {
                const char *test_results[] = {
                    "No test initiated",
                    "Test passed",
                    "Test in progress",
                    "General test failed",
                    "Battery failed",
                    "Deep battery test failed",
                    "Test aborted"
                };
                uint8_t test_val = data[1];
                if (test_val < 7) {
                    strncpy(target->self_test_result, test_results[test_val], sizeof(target->self_test_result) - 1);
                    ESP_LOGI(TAG, "   └─ Result: %s", target->self_test_result);
                    updated = true;
                }
            }
            break;

        case 0x1C:  // Battery manufacture date
            ESP_LOGI(TAG, "   Type: Battery Manufacture Date");
            if (length >= 4) {
                // Date stored as 3 bytes: Year (2 bytes), Month, Day
                uint16_t year = data[1] | (data[2] << 8);
                uint8_t month = (length > 3) ? data[3] : 1;
                uint8_t day = (length > 4) ? data[4] : 1;
                snprintf(target->battery_mfr_date, sizeof(target->battery_mfr_date),
                         "%04d/%02d/%02d", year, month, day);
                ESP_LOGI(TAG, "   └─ Date: %s", target->battery_mfr_date);
                updated = true;
            }
            break;

        case 0x20:  // Battery manufacture date (UPS.Battery.ManufacturerDate)
            ESP_LOGI(TAG, "   Type: Battery Manufacture Date");
            if (length >= 3) {
                // 16-bit value = days since reference date (likely 1970-01-01)
                // Example: 21690 days ≈ 59 years from 1970 = year 2029
                uint16_t days = data[1] | (data[2] << 8);
                snprintf(target->battery_mfr_date, sizeof(target->battery_mfr_date),
                         "%d days", days);
                ESP_LOGI(TAG, "   └─ Date: %d days since reference (raw data)", days);
                updated = true;
            }
            break;

        case 0x13:  // Delay before reboot (APCDelayBeforeReboot)
            ESP_LOGI(TAG, "   Type: Delay Before Reboot");
            if (length >= 2) {
                target->delay_before_reboot = (float)data[1];
                ESP_LOGI(TAG, "   └─ Delay: %.0f seconds", target->delay_before_reboot);
                updated = true;
            }
            break;

        case 0x14:
            if (is_smt2200_profile()) {
                ESP_LOGI(TAG, "   Type: SMT2200 Audible Alarm / Beeper");
                if (length >= 2) {
                    uint8_t beeper_val = data[1];  // confirmed sample 14 02
                    const char *state = "unknown";
                    if (beeper_val == 1) state = "disabled";
                    else if (beeper_val == 2) state = "enabled";
                    else if (beeper_val == 3) state = "muted";
                    strlcpy(target->beeper_status, state, sizeof(target->beeper_status));
                    ESP_LOGI(TAG, "   └─ Beeper enum %u → %s", beeper_val, target->beeper_status);
                    updated = true;
                }
            } else {
                ESP_LOGI(TAG, "   Type: Delay Before Shutdown");
                if (length >= 2) {
                    target->delay_before_shutdown = (float)data[1];
                    ESP_LOGI(TAG, "   └─ Delay: %.0f seconds", target->delay_before_shutdown);
                    updated = true;
                }
            }
            break;

        case 0x24:  // Battery runtime low threshold (UPS.Battery.RemainingTimeLimit)
            ESP_LOGI(TAG, "   Type: Battery Runtime Low Threshold");
            if (length >= 3) {
                uint16_t runtime = data[1] | (data[2] << 8);
                target->low_battery_runtime_threshold = (float)runtime;
                ESP_LOGI(TAG, "   └─ Threshold: %d seconds (%.1f min)", runtime, (float)runtime / 60.0f);
                updated = true;
            }
            break;

        case 0x21:  // Last transfer reason
            ESP_LOGI(TAG, "   Type: Last Transfer Reason");
            if (length >= 2) {
                const char *reasons[] = {
                    "No transfer",
                    "High line voltage",
                    "Brownout",
                    "Blackout",
                    "Small momentary sag",
                    "Deep momentary sag",
                    "Small momentary spike",
                    "Large momentary spike",
                    "Self test",
                    "Input frequency out of range",
                    "Input voltage out of range"
                };
                uint8_t reason = data[1];
                if (reason < 11) {
                    strncpy(target->last_transfer_reason, reasons[reason], sizeof(target->last_transfer_reason) - 1);
                    ESP_LOGI(TAG, "   └─ Reason code %d: %s", reason, target->last_transfer_reason);
                    updated = true;
                } else {
                    ESP_LOGI(TAG, "   └─ Unknown reason code: %d", reason);
                }
            }
            break;

        case 0x25:  // Nominal power
            ESP_LOGI(TAG, "   Type: Nominal Power");
            if (length >= 3) {
                uint16_t power = data[1] | (data[2] << 8);
                target->nominal_power = (float)power;
                ESP_LOGI(TAG, "   └─ Power: %.0fW", target->nominal_power);
                updated = true;
            }
            break;

        case 0x30:  // Input nominal voltage (UPS.Input.ConfigVoltage) - Feature Report
            ESP_LOGI(TAG, "   Type: Input Nominal Voltage");
            if (length >= 2) {
                target->input_voltage_nominal = (float)data[1];
                ESP_LOGI(TAG, "   └─ Nominal: %.0fV", target->input_voltage_nominal);
                updated = true;
            }
            break;

        case 0x31:  // Input voltage (UPS.Input.Voltage) - Feature Report, 16-bit LE, exponent -2
            ESP_LOGI(TAG, "   Type: Input Voltage");
            if (length >= 3) {
                uint16_t voltage_raw = data[1] | (data[2] << 8);
                target->input_voltage = (float)voltage_raw / 100.0f;
                ESP_LOGI(TAG, "   └─ Input: %.1fV (0x%04X / 100)", target->input_voltage, voltage_raw);
                updated = true;
            }
            break;

        case 0x32:  // Low voltage transfer (UPS.Input.LowVoltageTransfer) - Feature Report, 16-bit LE
            ESP_LOGI(TAG, "   Type: Low Voltage Transfer");
            if (length >= 3) {
                uint16_t voltage_raw = data[1] | (data[2] << 8);
                target->low_voltage_transfer = (float)voltage_raw / 100.0f;
                ESP_LOGI(TAG, "   └─ Transfer point: %.1fV", target->low_voltage_transfer);
                updated = true;
            }
            break;

        case 0x33:  // High voltage transfer (UPS.Input.HighVoltageTransfer) - Feature Report, 16-bit LE
            ESP_LOGI(TAG, "   Type: High Voltage Transfer");
            if (length >= 3) {
                uint16_t voltage_raw = data[1] | (data[2] << 8);
                target->high_voltage_transfer = (float)voltage_raw / 100.0f;
                ESP_LOGI(TAG, "   └─ Transfer point: %.1fV", target->high_voltage_transfer);
                updated = true;
            }
            break;

        case 0x50:  // Load percentage (UPS.PowerConverter.PercentLoad) - Feature Report
            ESP_LOGI(TAG, "   Type: Load Percentage");
            if (length >= 3) {
                // Try 16-bit first (some models report hundredths)
                uint16_t raw16 = data[1] | (data[2] << 8);
                float load_16 = (float)raw16 / 100.0f;
                float load_8 = (float)data[1];
                // If 16-bit/100 gives a sane % (0-100), use it; else use 8-bit
                if (load_16 >= 0.0f && load_16 <= 100.0f) {
                    target->load_percent = load_16;
                    ESP_LOGI(TAG, "   └─ Load: %.1f%% (16-bit 0x%04X / 100)", target->load_percent, raw16);
                } else {
                    target->load_percent = load_8;
                    ESP_LOGI(TAG, "   └─ Load: %.0f%% (8-bit 0x%02X)", target->load_percent, data[1]);
                }
                updated = true;
            } else if (length >= 2) {
                target->load_percent = (float)data[1];
                ESP_LOGI(TAG, "   └─ Load: %.0f%% (8-bit)", target->load_percent);
                updated = true;
            }
            break;

        case 0x35:  // Input sensitivity
            ESP_LOGI(TAG, "   Type: Input Sensitivity");
            if (length >= 2) {
                const char *sensitivity[] = {"low", "medium", "high"};
                uint8_t sens_val = data[1];
                if (sens_val < 3) {
                    strncpy(target->input_sensitivity, sensitivity[sens_val], sizeof(target->input_sensitivity) - 1);
                    ESP_LOGI(TAG, "   └─ Sensitivity: %s", target->input_sensitivity);
                    updated = true;
                }
            }
            break;

        case 0x36:  // Input frequency (UPS.Input.Frequency) - Feature Report
            ESP_LOGI(TAG, "   Type: Input Frequency");
            if (length >= 2) {
                // Frequency is typically 50 or 60 Hz
                // NOTE: This UPS reports 0 Hz - frequency might not be available
                // or encoded differently. Only update if non-zero.
                if (data[1] > 0) {
                    target->input_frequency = (float)data[1];
                    ESP_LOGI(TAG, "   └─ Frequency: %.0fHz", target->input_frequency);
                    updated = true;
                } else {
                    ESP_LOGI(TAG, "   └─ Frequency: Not available (0 Hz reported)");
                }
            }
            break;

        case 0x03:  // Battery chemistry/type (UPS.PowerSummary.iDeviceChemistry)
            ESP_LOGI(TAG, "   Type: Battery Chemistry");
            if (length >= 2) {
                // Common values: 1=PbAc (Lead Acid), 2=Li-ion, 3=NiCd, 4=NiMH
                // NOTE: APC typically uses PbAc but this UPS reports code 4 (NiMH)
                // Mapping might be vendor-specific. Reporting as-is.
                const char *chemistry[] = {"Unknown", "PbAc", "Li-ion", "NiCd", "NiMH"};
                uint8_t chem_val = data[1];
                if (chem_val < 5) {
                    strncpy(target->battery_type, chemistry[chem_val], sizeof(target->battery_type) - 1);
                    ESP_LOGI(TAG, "   └─ Chemistry code: %d → %s", chem_val, target->battery_type);
                    updated = true;
                } else {
                    ESP_LOGW(TAG, "   └─ Unknown chemistry code: %d", chem_val);
                }
            }
            break;

        case 0x07:  // UPS manufacture date (or unknown field)
            ESP_LOGI(TAG, "   Type: UPS Manufacture Date");
            // NOTE: This report returns only 3 bytes: 07 D6 54
            // Interpreting as date gives nonsensical year 21718
            // Likely this is NOT a date field or uses different encoding
            // Same data as Report 0x20 - both appear to be unidentified fields
            if (length >= 3) {
                ESP_LOGI(TAG, "   └─ Raw data: 0x%02X%02X%02X (unimplemented)",
                         data[1], data[2], (length > 2) ? data[3] : 0);
                ESP_LOGW(TAG, "   └─ Date parsing not implemented (insufficient data or wrong field)");
            }
            break;

        case 0x34:  // Input sensitivity adjustment (writable setting)
            ESP_LOGI(TAG, "   Type: Input Sensitivity Adjustment");
            if (length >= 2) {
                ESP_LOGI(TAG, "   └─ Adjustment value: %d", data[1]);
                updated = true;
            }
            break;

        case 0x52:  // Nominal real power (UPS.PowerSummary.ConfigActivePower)
            ESP_LOGI(TAG, "   Type: Nominal Real Power");
            if (length >= 3) {
                uint16_t power = data[1] | (data[2] << 8);
                target->nominal_power = (float)power;
                ESP_LOGI(TAG, "   └─ Real Power: %.0fW", target->nominal_power);
                updated = true;
            }
            break;

        case 0x60:  // Firmware version (part of string)
            ESP_LOGI(TAG, "   Type: Firmware Version");
            if (length >= 2) {
                // Firmware version is often split across multiple reports or encoded
                snprintf(target->firmware_version, sizeof(target->firmware_version), "%d.%d", data[1], data[2]);
                ESP_LOGI(TAG, "   └─ Version: %s", target->firmware_version);
                updated = true;
            }
            break;

        default:
            ESP_LOGI(TAG, "   Type: ❓ UNKNOWN Report ID (0x%02X)", report_id);
            ESP_LOGI(TAG, "   └─ This report ID is not yet handled");
            break;
    }

    if (updated) {
        target->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        target->valid = true;

        // Update status string
        apc_hid_format_status(&target->status, target->status_string,
                             sizeof(target->status_string));

        ESP_LOGI(TAG, "✅ METRICS UPDATED");
        ESP_LOGI(TAG, "   Status: %s", target->status_string);
        ESP_LOGI(TAG, "═══════════════════════════════════════════");

        // If we updated current_metrics, copy to output if provided
        if (target == &current_metrics && metrics != NULL) {
            memcpy(metrics, &current_metrics, sizeof(ups_metrics_t));
        }
    } else {
        ESP_LOGI(TAG, "⚠️  NO UPDATE (insufficient data or parsing issue)");
        ESP_LOGI(TAG, "═══════════════════════════════════════════");
    }

    return updated;
}

const ups_metrics_t* apc_hid_get_metrics(void)
{
    return &current_metrics;
}

void apc_hid_format_status(const ups_status_t *status, char *buffer, size_t buffer_size)
{
    buffer[0] = '\0';

    if (status->online) {
        strncat(buffer, "OL", buffer_size - strlen(buffer) - 1);  // Online
    } else if (status->discharging) {
        strncat(buffer, "OB", buffer_size - strlen(buffer) - 1);  // On Battery
    }

    if (status->charging) {
        if (strlen(buffer) > 0) strncat(buffer, " ", buffer_size - strlen(buffer) - 1);
        strncat(buffer, "CHRG", buffer_size - strlen(buffer) - 1);
    }

    if (status->low_battery) {
        if (strlen(buffer) > 0) strncat(buffer, " ", buffer_size - strlen(buffer) - 1);
        strncat(buffer, "LB", buffer_size - strlen(buffer) - 1);
    }

    if (status->overload) {
        if (strlen(buffer) > 0) strncat(buffer, " ", buffer_size - strlen(buffer) - 1);
        strncat(buffer, "OVER", buffer_size - strlen(buffer) - 1);
    }

    if (status->replace_battery) {
        if (strlen(buffer) > 0) strncat(buffer, " ", buffer_size - strlen(buffer) - 1);
        strncat(buffer, "RB", buffer_size - strlen(buffer) - 1);
    }

    if (status->boost) {
        if (strlen(buffer) > 0) strncat(buffer, " ", buffer_size - strlen(buffer) - 1);
        strncat(buffer, "BOOST", buffer_size - strlen(buffer) - 1);
    }

    if (status->trim) {
        if (strlen(buffer) > 0) strncat(buffer, " ", buffer_size - strlen(buffer) - 1);
        strncat(buffer, "TRIM", buffer_size - strlen(buffer) - 1);
    }

    if (strlen(buffer) == 0) {
        strncat(buffer, "UNKNOWN", buffer_size - 1);
    }
}


