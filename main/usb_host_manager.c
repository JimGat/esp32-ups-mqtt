/*
 * ═══════════════════════════════════════════════════════════════════════════
 * APC UPS USB HOST MANAGER - Comprehensive Architecture Overview
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * PURPOSE:
 * This module handles USB communication with an APC Back-UPS via USB HID protocol.
 * The ESP32-S3 acts as a USB HOST (like your computer), and the UPS acts as a
 * USB DEVICE (like a keyboard or mouse).
 *
 * WHY TWO TYPES OF USB TRANSFERS?
 * ─────────────────────────────────────────────────────────────────────────
 * 1. INTERRUPT TRANSFERS (Automatic, pushed by UPS):
 *    - The UPS automatically sends status updates every ~200-2000ms
 *    - These contain: battery charge, runtime, status bits (online/charging)
 *    - Think of this like the UPS "tapping you on the shoulder" with updates
 *    - Example reports: 0x06 (status), 0x0C (charge+runtime), 0x16 (detailed status)
 *
 * 2. CONTROL TRANSFERS / GET_REPORT (On-demand, we ask the UPS):
 *    - We must actively REQUEST certain data by asking for specific "Feature Reports"
 *    - These contain: voltage, load percentage, transfer thresholds
 *    - Think of this like us "asking a question" and waiting for the answer
 *    - Example reports: 0x09 (battery voltage), 0x31 (input voltage), 0x50 (load %)
 *
 * WHY DOESN'T THE UPS SEND EVERYTHING VIA INTERRUPTS?
 * ─────────────────────────────────────────────────────────────────────────
 * - USB HID devices categorize data into "Input Reports" (pushed) and
 *   "Feature Reports" (polled on request)
 * - Status data that changes frequently → Input Reports (interrupt transfers)
 * - Configuration/slow-changing data → Feature Reports (control transfers)
 * - This is standard HID behavior, not specific to APC
 *
 * THE CALLBACK MYSTERY - WHY WAS IT SO HARD?
 * ─────────────────────────────────────────────────────────────────────────
 * In ESP-IDF, USB transfers complete asynchronously via CALLBACKS:
 * - When you submit a transfer, it returns immediately (non-blocking)
 * - Later, when data arrives, a callback function fires
 * - BUT: Callbacks only fire when you call usb_host_xxx_handle_events()
 *
 * TWO LEVELS OF EVENT HANDLING (This was the key breakthrough!):
 * 1. usb_host_lib_handle_events()    - Library level (device connections, control transfers)
 * 2. usb_host_client_handle_events() - Client level (transfer completion callbacks)
 *
 * FOR INTERRUPT TRANSFERS: Only client events needed
 * FOR CONTROL TRANSFERS: BOTH lib AND client events needed
 *
 * This is why GET_REPORT was timing out - we weren't processing library events!
 *
 * THREAD SAFETY:
 * ─────────────────────────────────────────────────────────────────────────
 * - transfer_mutex: Prevents interrupt and control transfers from running simultaneously
 * - transfer_done: Semaphore to signal when a transfer callback has fired
 *
 * DATA FLOW:
 * ─────────────────────────────────────────────────────────────────────────
 * USB Device → Interrupt Transfer → Raw HID Report (bytes) →
 * apc_hid_parser.c (decode) → ups_metrics_t struct →
 * main.c (MQTT publish) → Home Assistant
 *
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include "usb_host_manager.h"
#include "apc_hid_parser.h"
#include "ups_hid_map.h"
#include "hid_descriptor_parser.h"
#include "nut_runtime_map.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"
#include <string.h>

static const char *TAG = "usb_host";
static usb_host_client_handle_t usb_client = NULL;  // Our USB client handle (declared before descriptor helper)

// Forward declarations used by the runtime USB debug command queue.
static esp_err_t get_hid_report_typed(uint8_t report_type, uint8_t report_id, uint8_t *buffer, size_t buffer_size, size_t *actual_length);
static void usb_debug_record_add(usb_debug_record_type_t type, uint8_t report_id, uint8_t status,
                                 const uint8_t *data, size_t len, const char *note);

static usb_debug_config_t debug_cfg = {
    .mode = USB_DEBUG_MODE_OFF,
    .capture_interrupt_reports = true,
    .capture_feature_reports = false,
    .include_control_setup = false,
    .log_to_esp_log = false,
};
// v0.4.14 Strict descriptor-first mode state machine
static volatile bool descriptor_needed = false;
static volatile bool descriptor_requested = false;
static volatile bool descriptor_complete = false;

/* ---- Evil Hardware Hacker Mode: Dump HID Report Descriptor ---- */
static void hid_report_desc_cb(usb_transfer_t *transfer) {
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        // usb_host_transfer_submit_control() leaves the 8-byte SETUP packet at
        // data_buffer[0..7], followed by the returned IN data. The default view
        // is usbhid-dump-style payload-only. If include_control_setup is enabled,
        // records intentionally include the raw SETUP packet for deep USB debugging.
        const int setup_len = sizeof(usb_setup_packet_t);
        int payload_len = transfer->actual_num_bytes - setup_len;
        if (payload_len < 0) payload_len = 0;

        bool include_setup = debug_cfg.include_control_setup;
        const uint8_t *record_data = include_setup ? transfer->data_buffer : (transfer->data_buffer + setup_len);
        int record_len = include_setup ? transfer->actual_num_bytes : payload_len;

        ESP_LOGI(TAG, "=== HID REPORT DESCRIPTOR payload=%d bytes raw_transfer=%d bytes view=%s ===",
                 payload_len, transfer->actual_num_bytes, include_setup ? "raw-control" : "payload-only");
        ESP_LOGI(TAG, "%s", include_setup ?
                 "Descriptor debug records include 8-byte control SETUP packet" :
                 "Descriptor setup packet stripped from debug records (8 bytes)");
        for (int i = 0; i < record_len; i += 16) {
            char line[80];
            int offset = 0;
            for (int j = 0; j < 16; j++) {
                if (i + j < record_len) {
                    offset += sprintf(&line[offset], "%02x ", record_data[i + j]);
                } else {
                    offset += sprintf(&line[offset], "   ");
                }
            }
            ESP_LOGI(TAG, "%04x: %s", i, line);
        }
        for (int i = 0; i < record_len; i += USB_DEBUG_MAX_RECORD_DATA) {
            char note[80];
            int chunk = record_len - i;
            if (chunk > USB_DEBUG_MAX_RECORD_DATA) chunk = USB_DEBUG_MAX_RECORD_DATA;
            snprintf(note, sizeof(note), "%s offset %d", include_setup ? "descriptor raw" : "descriptor payload", i);
            usb_debug_record_add(USB_DEBUG_REC_DESCRIPTOR, 0, 0, record_data + i, chunk, note);
        }
        hid_descriptor_field_t fields[64];
        size_t field_count = 0;
        if (hid_descriptor_parse(transfer->data_buffer + setup_len, payload_len,
                                 fields, sizeof(fields) / sizeof(fields[0]), &field_count)) {
            ESP_LOGI(TAG, "NUT-HID: descriptor resolver found %u fields", (unsigned)field_count);

            nut_runtime_map_entry_t runtime_map[32];
            size_t runtime_count = nut_runtime_map_build(fields, field_count,
                                                         runtime_map, sizeof(runtime_map) / sizeof(runtime_map[0]));
            ESP_LOGI(TAG, "NUT-HID: runtime semantic map contains %u entries", (unsigned)runtime_count);
            char map_summary[96];
            snprintf(map_summary, sizeof(map_summary), "nut runtime map entries=%u", (unsigned)runtime_count);
            usb_debug_record_add(USB_DEBUG_REC_EVENT, 0, 0, NULL, 0, map_summary);

            for (size_t m = 0; m < runtime_count; m++) {
                ESP_LOGI(TAG, "NUT-MAP: %s %s report=0x%02X bit=%u size=%u path=%s",
                         nut_runtime_key_str(runtime_map[m].key),
                         hid_descriptor_report_type_str(runtime_map[m].report_type),
                         runtime_map[m].report_id, runtime_map[m].bit_offset,
                         runtime_map[m].bit_size, runtime_map[m].hid_path);
                char note[160];
                snprintf(note, sizeof(note), "map %s r=0x%02X b=%u s=%u",
                         nut_runtime_key_str(runtime_map[m].key), runtime_map[m].report_id,
                         runtime_map[m].bit_offset, runtime_map[m].bit_size);
                usb_debug_record_add(USB_DEBUG_REC_EVENT, runtime_map[m].report_id, 0, NULL, 0, note);
            }

            for (size_t f = 0; f < field_count; f++) {
                if (fields[f].nut_name[0] != '\0' || strstr(fields[f].hid_path, "PresentStatus") != NULL) {
                    ESP_LOGI(TAG, "NUT-HID: %s report=0x%02X bit=%u size=%u path=%s nut=%s",
                             hid_descriptor_report_type_str(fields[f].type), fields[f].report_id,
                             fields[f].bit_offset, fields[f].bit_size, fields[f].hid_path, fields[f].nut_name);
                    char note[160];
                    const char *field_name = fields[f].nut_name[0] ? fields[f].nut_name : fields[f].hid_path;
                    snprintf(note, sizeof(note), "field r=0x%02X b=%u s=%u %s",
                             fields[f].report_id, fields[f].bit_offset, fields[f].bit_size, field_name);
                    usb_debug_record_add(USB_DEBUG_REC_EVENT, fields[f].report_id, 0, NULL, 0, note);
                }
            }
        } else {
            ESP_LOGW(TAG, "NUT-HID: descriptor parser failed; keeping raw dump only");
            usb_debug_record_add(USB_DEBUG_REC_ERROR, 0, 0, NULL, 0, "descriptor parse failed");
        }
        char summary[96];
        snprintf(summary, sizeof(summary), "descriptor complete payload=%d raw=%d fields=%u",
                 payload_len, transfer->actual_num_bytes, (unsigned)field_count);
        usb_debug_record_add(USB_DEBUG_REC_EVENT, 0, 0, NULL, 0, summary);
    } else {
        ESP_LOGE(TAG, "Failed to get HID Report Descriptor: %d", transfer->status);
        usb_debug_record_add(USB_DEBUG_REC_ERROR, 0, transfer->status, NULL, 0, "descriptor failed");
    }
    usb_host_transfer_free(transfer);
}

static void request_hid_report_descriptor(usb_device_handle_t dev_hdl, uint8_t intf_num) {
    usb_transfer_t *ctrl_xfer = NULL;
    const size_t payload_len = 1024;  // Some Smart-UPS descriptors exceed 512 bytes
    const size_t xfer_size = sizeof(usb_setup_packet_t) + payload_len;
    const size_t alloc_size = (xfer_size + 63) & ~((size_t)63);  // ESP-IDF USB host wants 64-byte aligned allocation sizes

    esp_err_t err = usb_host_transfer_alloc(alloc_size, 0, &ctrl_xfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to alloc descriptor ctrl xfer: %s", esp_err_to_name(err));
        usb_debug_record_add(USB_DEBUG_REC_ERROR, 0, err, NULL, 0, "descriptor alloc failed");
        return;
    }
    memset(ctrl_xfer->data_buffer, 0, alloc_size);

    ctrl_xfer->device_handle = dev_hdl;
    ctrl_xfer->bEndpointAddress = 0;
    ctrl_xfer->callback = hid_report_desc_cb;
    ctrl_xfer->context = NULL;
    ctrl_xfer->timeout_ms = 3000;
    ctrl_xfer->num_bytes = xfer_size;

    // Standard GET_DESCRIPTOR(HID Report, type 0x22) against interface 0.
    // Use the official setup packet layout but keep allocation aligned above.
    usb_setup_packet_t *setup = (usb_setup_packet_t *)ctrl_xfer->data_buffer;
    setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    setup->bRequest = USB_B_REQUEST_GET_DESCRIPTOR;
    setup->wValue = (0x22 << 8) | 0;
    setup->wIndex = intf_num;
    setup->wLength = payload_len;

    usb_debug_record_add(USB_DEBUG_REC_EVENT, 0, 0, NULL, 0, "descriptor submit");
    err = usb_host_transfer_submit_control(usb_client, ctrl_xfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to submit descriptor ctrl xfer: %s", esp_err_to_name(err));
        usb_debug_record_add(USB_DEBUG_REC_ERROR, 0, err, NULL, 0, "descriptor submit failed");
        usb_host_transfer_free(ctrl_xfer);
    } else {
        ESP_LOGI(TAG, "🔍 Requesting HID Report Descriptor from UPS...");
    }
}
/* ----------------------------------------------------------------------- */

//══════════════════════════════════════════════════════════════════════════════
// USB DEVICE IDENTIFICATION
//══════════════════════════════════════════════════════════════════════════════
// APC UPS USB Vendor/Product IDs (identifies this specific UPS model)
// VID 0x051D = American Power Conversion
// PID 0x0002 = Back-UPS series (Back-UPS XS 1000M, etc.)
// PID 0x0003 = Smart-UPS series (Smart-UPS C 1500, etc.)
#define APC_VID 0x051d
#define APC_PID_BACKUPS  0x0002
#define APC_PID_SMARTUPS 0x0003
#define IS_APC_UPS(vid, pid) ((vid) == APC_VID && ((pid) == APC_PID_BACKUPS || (pid) == APC_PID_SMARTUPS))

static ups_profile_t configured_ups_profile = UPS_PROFILE_AUTO;
static ups_profile_t active_ups_profile = UPS_PROFILE_APC_GENERIC_HID;

static const uint8_t smt2200_poll_reports[] __attribute__((unused)) = {
    0x09,  // SMT2200 PresentStatus-style bitfield; confirmed online sample 09 A8 4A
    0x0C,  // RemainingCapacity / battery charge; confirmed 0C 64 = 100%
    0x0A,  // RunTimeToEmpty; confirmed 0A C0 12 = 4800s
    0x08,  // Load percent; confirmed 08 78 00 = 12.0% (/10)
    0x0D,  // Battery voltage-like report; confirmed 0D B0 13, scale pending
    0x0B,  // Voltage/config-like report; confirmed 0B 4A 15, scale pending
    0x11,  // RemainingCapacityLimit / low charge threshold; confirmed 11 0A = 10%
    0x14,  // AudibleAlarmControl / beeper enum; confirmed 14 02
};

static const uint8_t generic_apc_poll_reports[] __attribute__((unused)) = {
    0x0A,  // Battery runtime (seconds, 16-bit LE)
    0x06,  // Generic APC status flags
    0x0D,  // Battery voltage (16-bit LE / 100)
    0x08,  // Battery nominal/config voltage
    0x0E,  // Full charge capacity
    0x0F,  // Battery warning threshold
    0x11,  // Battery low charge threshold
    0x03,  // Battery chemistry type
    0x07,  // UPS manufacture date
    0x10,  // Generic beeper status
};

void usb_host_set_configured_profile(ups_profile_t profile)
{
    configured_ups_profile = ups_profile_validate((uint8_t)profile);
    active_ups_profile = (configured_ups_profile == UPS_PROFILE_AUTO) ? UPS_PROFILE_APC_GENERIC_HID : configured_ups_profile;
    ESP_LOGI(TAG, "USB profile configured=%s active=%s",
             ups_profile_name(configured_ups_profile), ups_profile_name(active_ups_profile));
}

ups_profile_t usb_host_get_active_profile(void)
{
    return active_ups_profile;
}

static void resolve_usb_profile(uint16_t vid, uint16_t pid)
{
    ups_profile_t resolved = configured_ups_profile;
    if (resolved == UPS_PROFILE_AUTO) {
        // Current best-effort auto detection: APC VID 051D + PID 0003 is Jim's
        // Smart-UPS SMT2200 class. Fall back to generic APC HID otherwise.
        resolved = (vid == APC_VID && pid == APC_PID_SMARTUPS)
                   ? UPS_PROFILE_APC_SMT2200
                   : UPS_PROFILE_APC_GENERIC_HID;
    }
    active_ups_profile = resolved;
    apc_hid_parser_set_profile(active_ups_profile);
    ESP_LOGI(TAG, "🔎 UPS profile resolved: configured=%s active=%s (VID:PID=%04X:%04X)",
             ups_profile_name(configured_ups_profile), ups_profile_name(active_ups_profile), vid, pid);
}

//══════════════════════════════════════════════════════════════════════════════
// USB HOST STATE TRACKING
//══════════════════════════════════════════════════════════════════════════════
// These variables track the current state of the USB connection
static bool ups_connected = false;           // Is UPS physically connected?
static SemaphoreHandle_t usb_mutex = NULL;   // Mutex for USB library access
static usb_device_handle_t ups_device = NULL;       // Handle to the UPS device
static volatile bool client_events_observe_only_diag = false;
static volatile bool device_descriptor_only_diag = false;
static volatile bool config_descriptor_only_diag = false;
static volatile bool interface_claim_only_diag = false;
static volatile uint32_t diag_new_dev_count = 0;
static volatile uint32_t diag_dev_gone_count = 0;

// Runtime USB debug mode. This is intentionally NOT persisted to NVS:
// every reboot returns the bridge to normal MQTT mode so field debug cannot
// accidentally strand the device in a noisy diagnostic state.
#define USB_DEBUG_RING_SIZE 64
#define USB_DEBUG_CMD_QUEUE_LEN 4

typedef enum {
    USB_DEBUG_CMD_DESCRIPTOR = 1,
    USB_DEBUG_CMD_GET_REPORT = 2,
} usb_debug_cmd_type_t;

typedef struct {
    usb_debug_cmd_type_t type;
    uint8_t report_type;   // HID: 1=input, 2=output, 3=feature
    uint8_t report_id;
    uint16_t length;
} usb_debug_cmd_t;

static usb_debug_state_t debug_state = {0};
static SemaphoreHandle_t debug_mutex = NULL;
static QueueHandle_t debug_cmd_queue = NULL;
static usb_debug_record_t debug_ring[USB_DEBUG_RING_SIZE];
static uint32_t debug_next_seq = 1;
static size_t debug_head = 0;
static size_t debug_count = 0;


static void usb_debug_reset_command_queue(void)
{
    if (debug_cmd_queue != NULL) {
        xQueueReset(debug_cmd_queue);
    }
}

// Guard manual GET_REPORT lengths. The ESP32 debug UI is for discovery, but
// unsupported reports on slow UPS controllers can wedge if probed with large
// arbitrary lengths. These sizes include the report ID byte and are derived
// from the SMT2200 HID descriptor plus confirmed samples. Unknown reports get
// a conservative tiny read; vendor reports keep their descriptor lengths.
uint16_t usb_debug_safe_report_length(uint8_t report_id, uint16_t requested_length)
{
    // ESP-IDF/APC control reads are happier with a small padded transfer than
    // the literal descriptor minimum. Known tiny reports that return 2-3 bytes
    // are requested as 8 bytes; the actual returned length is still recorded.
    // Large vendor reports keep their descriptor length. Unknown reports use 8
    // rather than 64 to avoid the broad-sweep stall seen on report 0x08.
    uint16_t safe = 8;
    switch (report_id) {
        case 0x89: case 0x90: case 0x96:
            safe = 64;
            break;
        default:
            safe = 8;
            break;
    }
    if (requested_length > 0 && requested_length < safe) {
        return requested_length;
    }
    return safe;
}

//══════════════════════════════════════════════════════════════════════════════
// HID (Human Interface Device) CONFIGURATION
//══════════════════════════════════════════════════════════════════════════════
// HID Interface: UPS uses interface 0 for all HID communication
#define HID_INTERFACE 0

// HID Interrupt Endpoint: 0x81 means IN endpoint 1 (device-to-host)
// This is where the UPS automatically sends status updates
#define HID_INTERRUPT_IN_EP 0x81

static const char *usb_debug_mode_name(usb_debug_mode_t mode)
{
    switch (mode) {
        case USB_DEBUG_MODE_PASSIVE: return "passive";
        case USB_DEBUG_MODE_ACTIVE: return "active";
        case USB_DEBUG_MODE_OFF:
        default: return "off";
    }
}

static void usb_debug_record_add(usb_debug_record_type_t type, uint8_t report_id, uint8_t status,
                                 const uint8_t *data, size_t len, const char *note)
{
    if (debug_mutex == NULL) return;
    if (xSemaphoreTake(debug_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;

    usb_debug_record_t *rec = &debug_ring[debug_head];
    memset(rec, 0, sizeof(*rec));
    rec->seq = debug_next_seq++;
    rec->timestamp_ms = esp_timer_get_time() / 1000;
    rec->type = type;
    rec->report_id = report_id;
    rec->status = status;
    rec->length = len;
    if (data && len > 0) {
        size_t copy_len = len > USB_DEBUG_MAX_RECORD_DATA ? USB_DEBUG_MAX_RECORD_DATA : len;
        memcpy(rec->data, data, copy_len);
    }
    if (note) strlcpy(rec->note, note, sizeof(rec->note));

    debug_head = (debug_head + 1) % USB_DEBUG_RING_SIZE;
    if (debug_count < USB_DEBUG_RING_SIZE) {
        debug_count++;
    } else {
        debug_state.dropped_records++;
    }
    debug_state.last_activity_ms = rec->timestamp_ms;
    if (type == USB_DEBUG_REC_INTERRUPT) debug_state.interrupt_reports_seen++;
    if (type == USB_DEBUG_REC_FEATURE) debug_state.feature_reports_seen++;
    if (type == USB_DEBUG_REC_DESCRIPTOR) debug_state.descriptor_dumps++;
    if (type == USB_DEBUG_REC_ERROR) debug_state.errors++;

    bool log_to_esp = debug_cfg.log_to_esp_log;
    xSemaphoreGive(debug_mutex);

    if (log_to_esp) {
        ESP_LOGI(TAG, "USBDBG seq=%lu type=%d rid=0x%02X status=%u len=%u note=%s",
                 (unsigned long)rec->seq, type, report_id, status, (unsigned)len, note ? note : "");
    }
}

esp_err_t usb_debug_set_config(const usb_debug_config_t *cfg)
{
    if (!cfg || debug_mutex == NULL) return ESP_ERR_INVALID_ARG;
    if (cfg->mode > USB_DEBUG_MODE_ACTIVE) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(debug_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    usb_debug_mode_t old_mode = debug_cfg.mode;
    debug_cfg = *cfg;
    if (debug_cfg.mode == USB_DEBUG_MODE_OFF) {
        debug_cfg.capture_interrupt_reports = false;
        debug_cfg.capture_feature_reports = false;
    }
    debug_state.mode = debug_cfg.mode;
    xSemaphoreGive(debug_mutex);

    if (old_mode != debug_cfg.mode || debug_cfg.mode == USB_DEBUG_MODE_OFF) {
        usb_debug_reset_command_queue();
    }

    char note[48];
    snprintf(note, sizeof(note), "mode=%s", usb_debug_mode_name(cfg->mode));
    usb_debug_record_add(USB_DEBUG_REC_EVENT, 0, 0, NULL, 0, note);
    ESP_LOGI(TAG, "🔬 USB debug mode changed: %s", usb_debug_mode_name(cfg->mode));
    return ESP_OK;
}

void usb_debug_get_config(usb_debug_config_t *cfg)
{
    if (!cfg) return;
    if (debug_mutex && xSemaphoreTake(debug_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        *cfg = debug_cfg;
        xSemaphoreGive(debug_mutex);
    } else {
        *cfg = debug_cfg;
    }
}

void usb_debug_get_state(usb_debug_state_t *state)
{
    if (!state) return;
    if (debug_mutex && xSemaphoreTake(debug_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        debug_state.ups_connected = ups_connected;
        debug_state.mode = debug_cfg.mode;
        *state = debug_state;
        xSemaphoreGive(debug_mutex);
    } else {
        *state = debug_state;
        state->ups_connected = ups_connected;
    }
}

size_t usb_debug_get_records(usb_debug_record_t *out, size_t max_records, uint32_t since_seq)
{
    if (!out || max_records == 0 || debug_mutex == NULL) return 0;
    if (xSemaphoreTake(debug_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return 0;

    size_t copied = 0;
    size_t start = (debug_head + USB_DEBUG_RING_SIZE - debug_count) % USB_DEBUG_RING_SIZE;
    size_t available = debug_count;

    // With no explicit cursor, return the latest small window instead of the
    // oldest boot records. Keep the window small to avoid HTTP heap/stack spikes
    // while still surfacing descriptor summary and field records after a dump.
    if (since_seq == 0 && available > max_records) {
        start = (debug_head + USB_DEBUG_RING_SIZE - max_records) % USB_DEBUG_RING_SIZE;
        available = max_records;
    }

    for (size_t i = 0; i < available && copied < max_records; i++) {
        size_t idx = (start + i) % USB_DEBUG_RING_SIZE;
        if (since_seq == 0 || debug_ring[idx].seq > since_seq) {
            out[copied++] = debug_ring[idx];
        }
    }
    xSemaphoreGive(debug_mutex);
    return copied;
}

void usb_debug_clear_records(void)
{
    if (debug_mutex && xSemaphoreTake(debug_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset(debug_ring, 0, sizeof(debug_ring));
        debug_head = 0;
        debug_count = 0;
        debug_next_seq = 1;
        debug_state.dropped_records = 0;
        xSemaphoreGive(debug_mutex);
    }
    usb_debug_reset_command_queue();
}

esp_err_t usb_debug_request_descriptor(void)
{
    if (debug_cmd_queue == NULL) return ESP_ERR_INVALID_STATE;
    usb_debug_cmd_t cmd = { .type = USB_DEBUG_CMD_DESCRIPTOR };
    if (xQueueSend(debug_cmd_queue, &cmd, pdMS_TO_TICKS(50)) == pdTRUE) {
        usb_debug_record_add(USB_DEBUG_REC_EVENT, 0, 0, NULL, 0, "descriptor queued");
        return ESP_OK;
    }
    usb_debug_record_add(USB_DEBUG_REC_ERROR, 0, 7, NULL, 0, "descriptor queue full");
    return ESP_ERR_TIMEOUT;
}

static esp_err_t usb_debug_request_report_internal(uint8_t report_type, uint8_t report_id, uint16_t length, bool safe)
{
    if (debug_cmd_queue == NULL) return ESP_ERR_INVALID_STATE;
    if (report_type < 1 || report_type > 3 || length == 0 || length > 128) return ESP_ERR_INVALID_ARG;
    uint16_t req_len = safe ? usb_debug_safe_report_length(report_id, length) : length;
    if (req_len == 0 || req_len > 128) return ESP_ERR_INVALID_ARG;
    usb_debug_cmd_t cmd = {
        .type = USB_DEBUG_CMD_GET_REPORT,
        .report_type = report_type,
        .report_id = report_id,
        .length = req_len,
    };
    if (xQueueSend(debug_cmd_queue, &cmd, pdMS_TO_TICKS(50)) == pdTRUE) {
        char note[64];
        snprintf(note, sizeof(note), "GET_REPORT type=%u queued len=%u%s", report_type, req_len, safe ? " safe" : "");
        usb_debug_record_add(USB_DEBUG_REC_EVENT, report_id, 0, NULL, 0, note);
        return ESP_OK;
    }
    usb_debug_record_add(USB_DEBUG_REC_ERROR, report_id, 7, NULL, 0, "GET_REPORT queue full");
    return ESP_ERR_TIMEOUT;
}

esp_err_t usb_debug_request_report(uint8_t report_type, uint8_t report_id, uint16_t length)
{
    return usb_debug_request_report_internal(report_type, report_id, length, false);
}

esp_err_t usb_debug_request_report_safe(uint8_t report_type, uint8_t report_id, uint16_t requested_length)
{
    return usb_debug_request_report_internal(report_type, report_id, requested_length, true);
}

// USB Host client event handler
static void usb_host_client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    ESP_LOGI(TAG, "DEBUG: Event callback triggered, event=%d", event_msg->event);

    if (client_events_observe_only_diag || device_descriptor_only_diag || config_descriptor_only_diag || interface_claim_only_diag) {
        if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
            diag_new_dev_count++;
            ESP_LOGW(TAG, "USB_DIAG: NEW_DEV #%lu addr=%d observed", (unsigned long)diag_new_dev_count, event_msg->new_dev.address);
            if (device_descriptor_only_diag || config_descriptor_only_diag || interface_claim_only_diag) {
                usb_device_handle_t dev_hdl = NULL;
                esp_err_t err = usb_host_device_open(usb_client, event_msg->new_dev.address, &dev_hdl);
                ESP_LOGI(TAG, "USB_DEVICE_DESC_ONLY_DIAG: usb_host_device_open -> %s", esp_err_to_name(err));
                if (err == ESP_OK) {
                    const usb_device_desc_t *dev_desc = NULL;
                    err = usb_host_get_device_descriptor(dev_hdl, &dev_desc);
                    ESP_LOGI(TAG, "USB_DEVICE_DESC_ONLY_DIAG: usb_host_get_device_descriptor -> %s", esp_err_to_name(err));
                    if (err == ESP_OK && dev_desc != NULL) {
                        ESP_LOGI(TAG, "USB_DEVICE_DESC_ONLY_DIAG: VID:PID=%04X:%04X class=0x%02X configs=%d",
                                 dev_desc->idVendor,
                                 dev_desc->idProduct,
                                 dev_desc->bDeviceClass,
                                 dev_desc->bNumConfigurations);
                    }
                    if (config_descriptor_only_diag || interface_claim_only_diag) {
                        const usb_config_desc_t *config_desc = NULL;
                        err = usb_host_get_active_config_descriptor(dev_hdl, &config_desc);
                        ESP_LOGI(TAG, "USB_CONFIG_DESC_ONLY_DIAG: usb_host_get_active_config_descriptor -> %s", esp_err_to_name(err));
                        if (err == ESP_OK && config_desc != NULL) {
                            ESP_LOGI(TAG, "USB_CONFIG_DESC_ONLY_DIAG: bNumInterfaces=%d wTotalLength=%d configValue=%d",
                                     config_desc->bNumInterfaces,
                                     config_desc->wTotalLength,
                                     config_desc->bConfigurationValue);
                            int offset = 0;
                            const usb_intf_desc_t *intf = usb_parse_interface_descriptor(config_desc, HID_INTERFACE, 0, &offset);
                            if (intf != NULL) {
                                ESP_LOGI(TAG, "USB_CONFIG_DESC_ONLY_DIAG: intf%d class=0x%02X subclass=0x%02X protocol=0x%02X endpoints=%d",
                                         HID_INTERFACE,
                                         intf->bInterfaceClass,
                                         intf->bInterfaceSubClass,
                                         intf->bInterfaceProtocol,
                                         intf->bNumEndpoints);
                            } else {
                                ESP_LOGW(TAG, "USB_CONFIG_DESC_ONLY_DIAG: HID interface %d not found", HID_INTERFACE);
                            }
                        }
                    }
                    if (interface_claim_only_diag) {
                        err = usb_host_interface_claim(usb_client, dev_hdl, HID_INTERFACE, 0);
                        ESP_LOGI(TAG, "USB_INTERFACE_CLAIM_ONLY_DIAG: usb_host_interface_claim(intf=%d, alt=0) -> %s",
                                 HID_INTERFACE,
                                 esp_err_to_name(err));
                        if (err == ESP_OK) {
                            esp_err_t rel_err = usb_host_interface_release(usb_client, dev_hdl, HID_INTERFACE);
                            ESP_LOGI(TAG, "USB_INTERFACE_CLAIM_ONLY_DIAG: usb_host_interface_release(intf=%d) -> %s",
                                     HID_INTERFACE,
                                     esp_err_to_name(rel_err));
                        }
                    }
                    err = usb_host_device_close(usb_client, dev_hdl);
                    ESP_LOGI(TAG, "USB_DEVICE_DESC_ONLY_DIAG: usb_host_device_close -> %s", esp_err_to_name(err));
                }
            } else {
                ESP_LOGI(TAG, "USB_CLIENT_EVENTS_OBSERVE_DIAG: observe only; no open/claim/descriptor");
            }
        } else if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
            diag_dev_gone_count++;
            ESP_LOGW(TAG, "USB_DIAG: DEV_GONE #%lu observed; no cleanup work", (unsigned long)diag_dev_gone_count);
        } else {
            ESP_LOGI(TAG, "USB_DIAG: event=%d observed", event_msg->event);
        }
        return;
    }

    switch (event_msg->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV:
            ESP_LOGI(TAG, "🆕 New USB device detected (addr=%d)", event_msg->new_dev.address);

            // Open the device
            usb_device_handle_t dev_hdl;
            esp_err_t err = usb_host_device_open(usb_client, event_msg->new_dev.address, &dev_hdl);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to open device: %s", esp_err_to_name(err));
                break;
            }

            // Get device descriptor
            const usb_device_desc_t *dev_desc;
            err = usb_host_get_device_descriptor(dev_hdl, &dev_desc);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to get device descriptor: %s", esp_err_to_name(err));
                usb_host_device_close(usb_client, dev_hdl);
                break;
            }

            ESP_LOGI(TAG, "DEBUG: Device VID:PID = %04X:%04X", dev_desc->idVendor, dev_desc->idProduct);

            // Check if this is our APC UPS
            if (IS_APC_UPS(dev_desc->idVendor, dev_desc->idProduct)) {
                ESP_LOGI(TAG, "🔌 APC UPS found! VID:PID = %04X:%04X",
                         dev_desc->idVendor, dev_desc->idProduct);
                resolve_usb_profile(dev_desc->idVendor, dev_desc->idProduct);

                ups_device = dev_hdl;
                ups_connected = true;
                descriptor_needed = true;
                descriptor_requested = false;
                descriptor_complete = false;
                /* v0.4.14: Strict descriptor-first: flag only, no blocking in callback */
                ups_transport_stats_record_connected();

                // Claim HID interface FIRST (before inspecting)
                err = usb_host_interface_claim(usb_client, ups_device, HID_INTERFACE, 0);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to claim interface: %s", esp_err_to_name(err));
                } else {
                    ESP_LOGI(TAG, "✅ HID interface claimed successfully");
                    usb_debug_record_add(USB_DEBUG_REC_EVENT, 0, 0, NULL, 0, "interface claimed");
                }

                // Get configuration descriptor to inspect endpoints (after claiming)
                const usb_config_desc_t *config_desc;
                err = usb_host_get_active_config_descriptor(dev_hdl, &config_desc);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "📋 Config: %d interfaces", config_desc->bNumInterfaces);

                    // Parse interfaces and endpoints (don't claim again!)
                    int offset = 0;
                    const usb_intf_desc_t *intf = usb_parse_interface_descriptor(config_desc, HID_INTERFACE, 0, &offset);
                    if (intf) {
                        ESP_LOGI(TAG, "  Interface %d: class=0x%02X, endpoints=%d",
                                 HID_INTERFACE, intf->bInterfaceClass, intf->bNumEndpoints);

                        // Log endpoints
                        int ep_offset = offset;
                        for (int e = 0; e < intf->bNumEndpoints; e++) {
                            const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(intf, e, config_desc->wTotalLength, &ep_offset);
                            if (ep) {
                                ESP_LOGI(TAG, "    Endpoint 0x%02X: type=%d, maxPacket=%d",
                                         ep->bEndpointAddress,
                                         ep->bmAttributes & 0x03,
                                         ep->wMaxPacketSize);
                            }
                        }
                    }
                }
            } else {
                ESP_LOGI(TAG, "⚠️ Not an APC UPS (VID:PID = %04X:%04X), expected VID=%04X",
                         dev_desc->idVendor, dev_desc->idProduct, APC_VID);
                usb_host_device_close(usb_client, dev_hdl);
            }
            break;

        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            ESP_LOGW(TAG, "🚫 USB device removed");
            if (event_msg->dev_gone.dev_hdl == ups_device) {
                ups_connected = false;
                ups_device = NULL;
                descriptor_needed = false;
                descriptor_requested = false;
                descriptor_complete = false;
                /* v0.4: Record disconnection event */
                ups_transport_stats_record_disconnected();
                ESP_LOGI(TAG, "❌ APC UPS disconnected");
            }
            break;

        default:
            ESP_LOGI(TAG, "DEBUG: Unknown event %d", event_msg->event);
            break;
    }
}

//══════════════════════════════════════════════════════════════════════════════
// THREAD SYNCHRONIZATION FOR USB TRANSFERS
//══════════════════════════════════════════════════════════════════════════════

// transfer_done: Binary semaphore to signal when a USB transfer callback fires
// - We submit a transfer (returns immediately)
// - We wait on this semaphore
// - When data arrives, callback fires and gives this semaphore
// - We wake up and process the data
static SemaphoreHandle_t transfer_done;

// transfer_mutex: Prevents simultaneous interrupt + control transfers
// WHY NEEDED: The USB hardware can only handle one transfer at a time per endpoint
// - Interrupt transfers use endpoint 0x81
// - Control transfers use endpoint 0x00
// - But they share internal USB resources, so we serialize them
static SemaphoreHandle_t transfer_mutex;

//══════════════════════════════════════════════════════════════════════════════
// USB TRANSFER COMPLETION CALLBACK
//══════════════════════════════════════════════════════════════════════════════
// This function is called by the USB driver when a transfer completes
// CRITICAL: This runs in interrupt context, so keep it FAST and minimal
// - Just signal the semaphore
// - Don't do heavy processing here
// - Let the main task wake up and handle the data
static void transfer_callback(usb_transfer_t *transfer)
{
    // Signal that transfer is complete by "giving" the semaphore
    // The waiting task will wake up when it tries to "take" this semaphore
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(transfer_done, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

//══════════════════════════════════════════════════════════════════════════════
// GET_REPORT: REQUEST FEATURE REPORTS FROM THE UPS
//══════════════════════════════════════════════════════════════════════════════
// This function actively ASKS the UPS for specific data using HID GET_REPORT
//
// USB HID GET_REPORT Protocol:
// ────────────────────────────────────────────────────────────────────────────
// 1. We send a control transfer with:
//    - bmRequestType: 0xA1 (Device-to-Host, Class request, Interface recipient)
//    - bRequest: 0x01 (GET_REPORT - standard HID request)
//    - wValue: (ReportType << 8) | ReportID
//      * ReportType = 3 (Feature Report) - polled data like voltage, load
//      * ReportID = specific report we want (0x09=battery voltage, 0x31=input voltage, etc.)
//    - wIndex: 0 (HID interface number)
//    - wLength: How many bytes we expect back
//
// 2. UPS responds with the requested report data
//
// 3. Our callback fires when data arrives
//
// THE CRITICAL FIX - WHY TWO EVENT HANDLERS:
// ────────────────────────────────────────────────────────────────────────────
// In the wait loop, we MUST call BOTH:
// - usb_host_lib_handle_events()    → Processes control transfer at hardware level
// - usb_host_client_handle_events() → Fires our callback when data arrives
//
// If we only call client events (like we did initially), control transfers
// never complete because the library-level processing doesn't happen!
//
// This took HOURS to debug because:
// - Interrupt transfers only need client events
// - Control transfers need BOTH lib and client events
// - The documentation doesn't clearly explain this difference
//
static esp_err_t get_hid_report_typed(uint8_t report_type, uint8_t report_id, uint8_t *buffer, size_t buffer_size, size_t *actual_length)
{
    if (ups_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Acquire mutex: Only one USB transfer at a time
    // This prevents interrupt and control transfers from interfering
    if (xSemaphoreTake(transfer_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire transfer mutex for GET_REPORT");
        return ESP_ERR_TIMEOUT;
    }

    // Prepare USB transfer for control request. Allocate on a 64-byte boundary;
    // several ESP-IDF USB Host paths reject otherwise valid control transfers
    // with ESP_ERR_INVALID_ARG when the backing buffer is not packet-aligned.
    usb_transfer_t *transfer;
    const size_t xfer_size = buffer_size + sizeof(usb_setup_packet_t);
    const size_t alloc_size = (xfer_size + 63) & ~((size_t)63);
    esp_err_t err = usb_host_transfer_alloc(alloc_size, 0, &transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate control transfer: %s", esp_err_to_name(err));
        xSemaphoreGive(transfer_mutex);
        return err;
    }

    // Setup control GET_REPORT request
    // Request Type: 0xA1 = Device-to-Host, Class, Interface
    // Request: 0x01 = GET_REPORT
    // Value: (ReportType << 8) | ReportID, where ReportType=3 for Feature Report
    // Index: Interface number (0)
    // Length: Expected report size
    // NOTE: Changed from Input Reports (type 1) to Feature Reports (type 3)
    // because voltage/load/frequency are synchronous polled values, not async events
    transfer->device_handle = ups_device;
    transfer->bEndpointAddress = 0x00;  // Control endpoint
    transfer->callback = transfer_callback;
    transfer->context = NULL;
    memset(transfer->data_buffer, 0, alloc_size);
    transfer->num_bytes = xfer_size;
    transfer->timeout_ms = 3000;

    // Fill setup packet
    usb_setup_packet_t *setup = (usb_setup_packet_t *)transfer->data_buffer;
    setup->bmRequestType = 0xA1;  // Device-to-Host, Class, Interface
    setup->bRequest = 0x01;        // GET_REPORT
    setup->wValue = (report_type << 8) | report_id;  // Report type + Report ID
    setup->wIndex = HID_INTERFACE;
    setup->wLength = buffer_size;

    // Create semaphore if not already created
    if (transfer_done == NULL) {
        transfer_done = xSemaphoreCreateBinary();
        if (transfer_done == NULL) {
            ESP_LOGE(TAG, "Failed to create transfer semaphore");
            usb_host_transfer_free(transfer);
            xSemaphoreGive(transfer_mutex);
            return ESP_ERR_NO_MEM;
        }
    }

    // Submit transfer
    err = usb_host_transfer_submit_control(usb_client, transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to submit GET_REPORT for 0x%02X: %s", report_id, esp_err_to_name(err));
        usb_host_transfer_free(transfer);
        xSemaphoreGive(transfer_mutex);
        return err;
    }

    ESP_LOGD(TAG, "🔍 Requesting report ID 0x%02X...", report_id);

    // Wait for transfer completion
    // CRITICAL: Must process BOTH lib and client events for control transfers
    const int max_wait_ms = 4000;
    const int poll_interval_ms = 10;
    int waited_ms = 0;
    bool transfer_complete = false;

    // ═══════════════════════════════════════════════════════════════════════
    // THE CRITICAL WAIT LOOP - This is what makes control transfers work!
    // ═══════════════════════════════════════════════════════════════════════
    while (waited_ms < max_wait_ms && !transfer_complete) {
        // STEP 1: Process library-level events (hardware USB processing)
        // This is ESSENTIAL for control transfers to actually execute
        uint32_t event_flags;
        usb_host_lib_handle_events(pdMS_TO_TICKS(5), &event_flags);

        // STEP 2: Process client-level events (fires our callback)
        // This checks if our transfer_callback has been called
        usb_host_client_handle_events(usb_client, pdMS_TO_TICKS(5));

        // STEP 3: Check if callback fired (semaphore was given)
        // Non-blocking check (timeout=0) so we keep looping
        if (xSemaphoreTake(transfer_done, 0) == pdTRUE) {
            transfer_complete = true;
            break;
        }
        waited_ms += poll_interval_ms;
    }
    // ═══════════════════════════════════════════════════════════════════════

    if (transfer_complete) {
        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
            // Data starts after 8-byte setup packet
            *actual_length = transfer->actual_num_bytes - 8;
            if (*actual_length > 0 && *actual_length <= buffer_size) {
                memcpy(buffer, transfer->data_buffer + 8, *actual_length);
                ESP_LOGI(TAG, "✅ GET_REPORT 0x%02X: %d bytes", report_id, *actual_length);
                /* v0.4: Record transport stats */
                ups_transport_stats_record_get_report_success();
                err = ESP_OK;
            } else {
                err = ESP_ERR_INVALID_SIZE;
                /* v0.4: Record error */
                ups_transport_stats_record_get_report_error(transfer->status);
            }
        } else if (transfer->status == USB_TRANSFER_STATUS_STALL) {
            ESP_LOGI(TAG, "⚠️  Report 0x%02X not available (STALL)", report_id);
            /* v0.4: Record STALL */
            ups_transport_stats_record_get_report_stall();
            err = ESP_ERR_NOT_SUPPORTED;
        } else {
            ESP_LOGI(TAG, "⚠️  GET_REPORT 0x%02X failed, status=%d", report_id, transfer->status);
            /* v0.4: Record error */
            ups_transport_stats_record_get_report_error(transfer->status);
            err = ESP_FAIL;
        }
        usb_host_transfer_free(transfer);
    } else {
        ESP_LOGW(TAG, "⚠️  GET_REPORT 0x%02X timeout after %dms, aborting", report_id, max_wait_ms);
        ESP_LOGW(TAG, "   Transfer status: %d (0=no_device, 1=completed, 2=error, 3=timed_out, 4=cancelled, 5=stall, 6=overflow, 7=skipped)",
                 transfer->status);

        /* v0.4: Record timeout */
        ups_transport_stats_record_get_report_timeout();

        // Cancel and free the transfer
        // Don't wait forever - the UPS doesn't support this report ID
        usb_host_transfer_free(transfer);
        err = ESP_ERR_NOT_SUPPORTED;
    }

    // Release mutex
    xSemaphoreGive(transfer_mutex);
    return err;
}

static esp_err_t get_hid_report(uint8_t report_id, uint8_t *buffer, size_t buffer_size, size_t *actual_length)
{
    return get_hid_report_typed(3, report_id, buffer, buffer_size, actual_length);
}

// Read HID report from interrupt endpoint
static esp_err_t read_hid_report(uint8_t *buffer, size_t buffer_size, size_t *actual_length)
{
    if (ups_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Acquire mutex to prevent concurrent transfers
    if (xSemaphoreTake(transfer_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire transfer mutex for interrupt read");
        return ESP_ERR_TIMEOUT;
    }

    // Create semaphore if not already created
    if (transfer_done == NULL) {
        transfer_done = xSemaphoreCreateBinary();
        if (transfer_done == NULL) {
            ESP_LOGE(TAG, "Failed to create transfer semaphore");
            xSemaphoreGive(transfer_mutex);
            return ESP_ERR_NO_MEM;
        }
    }

    // Prepare USB transfer
    usb_transfer_t *transfer;
    esp_err_t err = usb_host_transfer_alloc(buffer_size, 0, &transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate transfer: %s", esp_err_to_name(err));
        xSemaphoreGive(transfer_mutex);
        return err;
    }

    // Setup interrupt IN transfer
    transfer->device_handle = ups_device;
    transfer->bEndpointAddress = HID_INTERRUPT_IN_EP;
    transfer->callback = transfer_callback;
    transfer->context = NULL;
    transfer->num_bytes = buffer_size;
    transfer->timeout_ms = 2000;  // Increased from 300ms - SMT2200 USB controller is notoriously slow

    // Submit transfer
    err = usb_host_transfer_submit(transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to submit transfer: %s", esp_err_to_name(err));
        usb_host_transfer_free(transfer);
        xSemaphoreGive(transfer_mutex);
        return err;
    }

    // Wait for transfer to complete while processing USB events
    // The callback can ONLY fire when usb_host_client_handle_events() is called
    // So we must poll events while waiting, not just block on semaphore
    ESP_LOGD(TAG, "⏳ Waiting for transfer completion (endpoint 0x%02X)...", HID_INTERRUPT_IN_EP);

    // Poll for up to 500ms (interrupt data comes every ~8s from UPS)
    // Shorter timeout means we return to the main loop faster for poll cycles
    const int max_wait_ms = 500;
    const int poll_interval_ms = 10;
    int waited_ms = 0;
    bool transfer_complete = false;

    while (waited_ms < max_wait_ms && !transfer_complete) {
        // Process USB client events - this is where callbacks fire!
        usb_host_client_handle_events(usb_client, pdMS_TO_TICKS(poll_interval_ms));

        // Check if semaphore was signaled (non-blocking check)
        if (xSemaphoreTake(transfer_done, 0) == pdTRUE) {
            transfer_complete = true;
            break;
        }

        waited_ms += poll_interval_ms;
    }

    if (transfer_complete) {
        ESP_LOGD(TAG, "🔔 Transfer callback fired, status=%d (after %dms)", transfer->status, waited_ms);

        // Copy data and get actual length
        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
            *actual_length = transfer->actual_num_bytes;
            memcpy(buffer, transfer->data_buffer, transfer->actual_num_bytes);

            if (*actual_length > 0) {
                ESP_LOGI(TAG, "✅ HID report received: %d bytes", *actual_length);
                ESP_LOG_BUFFER_HEX_LEVEL(TAG, buffer, (*actual_length < 16) ? *actual_length : 16, ESP_LOG_INFO);
            }
            err = ESP_OK;
        } else if (transfer->status == USB_TRANSFER_STATUS_TIMED_OUT) {
            ESP_LOGD(TAG, "⏱️  Transfer timed out (USB level) - device not sending data");
            err = ESP_ERR_TIMEOUT;
        } else if (transfer->status == USB_TRANSFER_STATUS_STALL) {
            ESP_LOGW(TAG, "⚠️  Transfer stalled - endpoint may not be ready");
            err = ESP_FAIL;
        } else if (transfer->status == USB_TRANSFER_STATUS_ERROR) {
            ESP_LOGW(TAG, "❌ Transfer error");
            err = ESP_FAIL;
        } else if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
            ESP_LOGW(TAG, "❌ Device disconnected");
            err = ESP_FAIL;
        } else {
            ESP_LOGW(TAG, "❌ Transfer failed with unknown status: %d", transfer->status);
            err = ESP_FAIL;
        }

        // CRITICAL: Only free after callback has fired
        usb_host_transfer_free(transfer);
    } else {
        // Timeout waiting for callback - release mutex so other transfers can proceed,
        // then keep processing events until callback fires to avoid memory corruption
        ESP_LOGW(TAG, "⚠️  App-level timeout (%dms), releasing mutex and waiting for late callback...", max_wait_ms);
        xSemaphoreGive(transfer_mutex);  // Release mutex BEFORE waiting

        // Keep processing events until callback fires (with a hard limit)
        int late_wait_ms = 0;
        const int LATE_WAIT_MAX_MS = 5000;  // Give up after 5s more
        while (!transfer_complete && late_wait_ms < LATE_WAIT_MAX_MS) {
            usb_host_client_handle_events(usb_client, pdMS_TO_TICKS(poll_interval_ms));
            if (xSemaphoreTake(transfer_done, 0) == pdTRUE) {
                transfer_complete = true;
                ESP_LOGW(TAG, "✅ Late callback received, freeing transfer (waited %dms more)", late_wait_ms);
                usb_host_transfer_free(transfer);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
            late_wait_ms += poll_interval_ms;
        }
        if (!transfer_complete) {
            ESP_LOGE(TAG, "❌ Late callback never fired after %dms, leaking transfer", late_wait_ms);
            // Can't free transfer without callback -- it's a small leak but prevents crash
        }
        return ESP_ERR_TIMEOUT;
    }

    // Release mutex
    xSemaphoreGive(transfer_mutex);
    return err;
}

static void usb_debug_process_commands(void)
{
    if (debug_cmd_queue == NULL) return;

    usb_debug_cmd_t cmd;
    while (xQueueReceive(debug_cmd_queue, &cmd, 0) == pdTRUE) {
        usb_debug_config_t cfg;
        usb_debug_get_config(&cfg);
        if (cfg.mode != USB_DEBUG_MODE_ACTIVE) {
            usb_debug_record_add(USB_DEBUG_REC_ERROR, 0, 0, NULL, 0, "debug inactive");
            continue;
        }
        if (!ups_connected || ups_device == NULL) {
            usb_debug_record_add(USB_DEBUG_REC_ERROR, 0, 0, NULL, 0, "UPS not connected");
            continue;
        }

        if (cmd.type == USB_DEBUG_CMD_DESCRIPTOR) {
            usb_debug_record_add(USB_DEBUG_REC_EVENT, 0, 0, NULL, 0, "descriptor processing");
            request_hid_report_descriptor(ups_device, HID_INTERFACE);
        } else if (cmd.type == USB_DEBUG_CMD_GET_REPORT) {
            uint8_t buf[128] = {0};
            size_t len = 0;
            usb_debug_record_add(USB_DEBUG_REC_EVENT, cmd.report_id, 0, NULL, 0, "GET_REPORT processing");
            esp_err_t err = get_hid_report_typed(cmd.report_type, cmd.report_id, buf, cmd.length, &len);
            if (err == ESP_OK) {
                usb_debug_record_add(USB_DEBUG_REC_FEATURE, cmd.report_id, 0, buf, len, "manual GET_REPORT");
            } else {
                usb_debug_record_add(USB_DEBUG_REC_ERROR, cmd.report_id, err, NULL, 0, esp_err_to_name(err));
            }
        }
    }
}



esp_err_t usb_host_register_client_only_diag(void)
{
    ESP_LOGI(TAG, "USB_CLIENT_ONLY_DIAG: install USB Host library and register client only");
    ESP_LOGI(TAG, "USB_CLIENT_ONLY_DIAG: no USB task, no event handling, no descriptor request");

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    esp_err_t ret = usb_host_install(&host_config);
    ESP_LOGI(TAG, "USB_CLIENT_ONLY_DIAG: usb_host_install returned: 0x%x (%s)", ret, esp_err_to_name(ret));
    if (ret != ESP_OK) {
        return ret;
    }

    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = usb_host_client_event_cb,
            .callback_arg = NULL
        }
    };

    ret = usb_host_client_register(&client_config, &usb_client);
    ESP_LOGI(TAG, "USB_CLIENT_ONLY_DIAG: usb_host_client_register returned: 0x%x (%s)", ret, esp_err_to_name(ret));
    return ret;
}

esp_err_t usb_host_install_only_diag(void)
{
    ESP_LOGI(TAG, "USB_INSTALL_ONLY_DIAG: installing USB Host library only");
    ESP_LOGI(TAG, "USB_INSTALL_ONLY_DIAG: no client registration, no USB task, no descriptor request");

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    esp_err_t ret = usb_host_install(&host_config);
    ESP_LOGI(TAG, "USB_INSTALL_ONLY_DIAG: usb_host_install returned: 0x%x (%s)", ret, esp_err_to_name(ret));
    return ret;
}

esp_err_t usb_host_init(void)
{
    ESP_LOGI(TAG, "DEBUG: usb_host_init() called");
    ESP_LOGI(TAG, "🚀 Initializing USB Host for APC UPS");
    ESP_LOGW(TAG, "⚠️ Note: Many ESP32-S3 dev boards don't expose USB OTG pins");

    // Create mutex
    ESP_LOGI(TAG, "DEBUG: Creating USB mutex");
    usb_mutex = xSemaphoreCreateMutex();
    if (usb_mutex == NULL) {
        ESP_LOGE(TAG, "❌ Failed to create USB mutex");
        return ESP_FAIL;
    }

    // Create transfer mutex to serialize transfers
    transfer_mutex = xSemaphoreCreateMutex();
    if (transfer_mutex == NULL) {
        ESP_LOGE(TAG, "❌ Failed to create transfer mutex");
        return ESP_FAIL;
    }

    debug_mutex = xSemaphoreCreateMutex();
    debug_cmd_queue = xQueueCreate(USB_DEBUG_CMD_QUEUE_LEN, sizeof(usb_debug_cmd_t));
    if (debug_mutex == NULL || debug_cmd_queue == NULL) {
        ESP_LOGE(TAG, "❌ Failed to create USB debug state/queue");
        return ESP_FAIL;
    }

    /* v0.4: Initialize transport health stats */
    ups_transport_stats_init();

    // Install USB Host library
    ESP_LOGI(TAG, "DEBUG: Installing USB Host library");
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    esp_err_t ret = usb_host_install(&host_config);
    ESP_LOGI(TAG, "DEBUG: usb_host_install returned: 0x%x", ret);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to install USB host: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "💡 Your board may not support USB OTG on external pins");
        ESP_LOGW(TAG, "📝 Continuing with simulated data...");
        return ret;
    }

    // Register USB host client
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = usb_host_client_event_cb,
            .callback_arg = NULL
        }
    };

    ret = usb_host_client_register(&client_config, &usb_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to register USB client: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "💡 USB OTG not available on this board");
        usb_host_uninstall();
        return ret;
    }

    ESP_LOGI(TAG, "✅ USB Host initialized successfully");
    ESP_LOGI(TAG, "🔍 Waiting for APC UPS (VID=%04X, PID=%04X or %04X)", APC_VID, APC_PID_BACKUPS, APC_PID_SMARTUPS);

    return ESP_OK;
}






void usb_host_interface_claim_only_task(void *arg)
{
    (void)arg;
    client_events_observe_only_diag = false;
    device_descriptor_only_diag = false;
    config_descriptor_only_diag = false;
    interface_claim_only_diag = true;
    diag_new_dev_count = 0;
    diag_dev_gone_count = 0;
    ESP_LOGI(TAG, "USB_INTERFACE_CLAIM_ONLY_DIAG: task started");
    ESP_LOGI(TAG, "USB_INTERFACE_CLAIM_ONLY_DIAG: callback opens device, reads descriptors, claims+releases HID interface, closes");

    int loop_count = 0;
    int error_count = 0;
    int64_t last_heartbeat_ms = 0;

    while (1) {
        loop_count++;

        uint32_t event_flags = 0;
        esp_err_t lib_err = usb_host_lib_handle_events(pdMS_TO_TICKS(10), &event_flags);
        if (lib_err != ESP_OK && lib_err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "USB_INTERFACE_CLAIM_ONLY_DIAG: lib event error: %s", esp_err_to_name(lib_err));
        }
        if (event_flags != 0) {
            ESP_LOGI(TAG, "USB_INTERFACE_CLAIM_ONLY_DIAG: lib event_flags=0x%lx", (unsigned long)event_flags);
        }

        esp_err_t client_err = usb_host_client_handle_events(usb_client, pdMS_TO_TICKS(10));
        if (client_err != ESP_OK && client_err != ESP_ERR_TIMEOUT) {
            error_count++;
            ESP_LOGW(TAG, "USB_INTERFACE_CLAIM_ONLY_DIAG: client event error %d: %s", error_count, esp_err_to_name(client_err));
        }

        int64_t now_ms = esp_timer_get_time() / 1000;
        if ((now_ms - last_heartbeat_ms) >= 10000) {
            last_heartbeat_ms = now_ms;
            ESP_LOGI(TAG, "USB_INTERFACE_CLAIM_ONLY_DIAG heartbeat: loop=%d errors=%d new_dev=%lu dev_gone=%lu",
                     loop_count,
                     error_count,
                     (unsigned long)diag_new_dev_count,
                     (unsigned long)diag_dev_gone_count);
            if (diag_new_dev_count == 0) {
                ESP_LOGW(TAG, "USB_INTERFACE_CLAIM_ONLY_DIAG: still waiting for NEW_DEV event from USB host client");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void usb_host_config_descriptor_only_task(void *arg)
{
    (void)arg;
    client_events_observe_only_diag = false;
    device_descriptor_only_diag = false;
    config_descriptor_only_diag = true;
    diag_new_dev_count = 0;
    diag_dev_gone_count = 0;
    ESP_LOGI(TAG, "USB_CONFIG_DESC_ONLY_DIAG: task started");
    ESP_LOGI(TAG, "USB_CONFIG_DESC_ONLY_DIAG: callback opens device, reads device+active config descriptors, closes");

    int loop_count = 0;
    int error_count = 0;
    int64_t last_heartbeat_ms = 0;

    while (1) {
        loop_count++;

        uint32_t event_flags = 0;
        esp_err_t lib_err = usb_host_lib_handle_events(pdMS_TO_TICKS(10), &event_flags);
        if (lib_err != ESP_OK && lib_err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "USB_CONFIG_DESC_ONLY_DIAG: lib event error: %s", esp_err_to_name(lib_err));
        }
        if (event_flags != 0) {
            ESP_LOGI(TAG, "USB_CONFIG_DESC_ONLY_DIAG: lib event_flags=0x%lx", (unsigned long)event_flags);
        }

        esp_err_t client_err = usb_host_client_handle_events(usb_client, pdMS_TO_TICKS(10));
        if (client_err != ESP_OK && client_err != ESP_ERR_TIMEOUT) {
            error_count++;
            ESP_LOGW(TAG, "USB_CONFIG_DESC_ONLY_DIAG: client event error %d: %s", error_count, esp_err_to_name(client_err));
        }

        int64_t now_ms = esp_timer_get_time() / 1000;
        if ((now_ms - last_heartbeat_ms) >= 10000) {
            last_heartbeat_ms = now_ms;
            ESP_LOGI(TAG, "USB_CONFIG_DESC_ONLY_DIAG heartbeat: loop=%d errors=%d new_dev=%lu dev_gone=%lu",
                     loop_count,
                     error_count,
                     (unsigned long)diag_new_dev_count,
                     (unsigned long)diag_dev_gone_count);
            if (diag_new_dev_count == 0) {
                ESP_LOGW(TAG, "USB_CONFIG_DESC_ONLY_DIAG: still waiting for NEW_DEV event from USB host client");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void usb_host_device_descriptor_only_task(void *arg)
{
    (void)arg;
    client_events_observe_only_diag = false;
    device_descriptor_only_diag = true;
    ESP_LOGI(TAG, "USB_DEVICE_DESC_ONLY_DIAG: task started");
    ESP_LOGI(TAG, "USB_DEVICE_DESC_ONLY_DIAG: client events enabled; callback opens device, reads device descriptor, closes");

    int loop_count = 0;
    int error_count = 0;
    int64_t last_heartbeat_ms = 0;

    while (1) {
        loop_count++;

        uint32_t event_flags = 0;
        esp_err_t lib_err = usb_host_lib_handle_events(pdMS_TO_TICKS(10), &event_flags);
        if (lib_err != ESP_OK && lib_err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "USB_DEVICE_DESC_ONLY_DIAG: lib event error: %s", esp_err_to_name(lib_err));
        }
        if (event_flags != 0) {
            ESP_LOGI(TAG, "USB_DEVICE_DESC_ONLY_DIAG: lib event_flags=0x%lx", (unsigned long)event_flags);
        }

        esp_err_t client_err = usb_host_client_handle_events(usb_client, pdMS_TO_TICKS(10));
        if (client_err != ESP_OK && client_err != ESP_ERR_TIMEOUT) {
            error_count++;
            ESP_LOGW(TAG, "USB_DEVICE_DESC_ONLY_DIAG: client event error %d: %s", error_count, esp_err_to_name(client_err));
        }

        int64_t now_ms = esp_timer_get_time() / 1000;
        if ((now_ms - last_heartbeat_ms) >= 2000) {
            last_heartbeat_ms = now_ms;
            ESP_LOGI(TAG, "USB_DEVICE_DESC_ONLY_DIAG heartbeat: loop=%d errors=%d", loop_count, error_count);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void usb_host_client_events_observe_task(void *arg)
{
    (void)arg;
    client_events_observe_only_diag = true;
    ESP_LOGI(TAG, "USB_CLIENT_EVENTS_OBSERVE_DIAG: task started");
    ESP_LOGI(TAG, "USB_CLIENT_EVENTS_OBSERVE_DIAG: pumping lib+client events; callback observes only");

    int loop_count = 0;
    int error_count = 0;
    int64_t last_heartbeat_ms = 0;

    while (1) {
        loop_count++;

        uint32_t event_flags = 0;
        esp_err_t lib_err = usb_host_lib_handle_events(pdMS_TO_TICKS(10), &event_flags);
        if (lib_err != ESP_OK && lib_err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "USB_CLIENT_EVENTS_OBSERVE_DIAG: lib event error: %s", esp_err_to_name(lib_err));
        }
        if (event_flags != 0) {
            ESP_LOGI(TAG, "USB_CLIENT_EVENTS_OBSERVE_DIAG: lib event_flags=0x%lx", (unsigned long)event_flags);
        }

        esp_err_t client_err = usb_host_client_handle_events(usb_client, pdMS_TO_TICKS(10));
        if (client_err != ESP_OK && client_err != ESP_ERR_TIMEOUT) {
            error_count++;
            ESP_LOGW(TAG, "USB_CLIENT_EVENTS_OBSERVE_DIAG: client event error %d: %s", error_count, esp_err_to_name(client_err));
        }

        int64_t now_ms = esp_timer_get_time() / 1000;
        if ((now_ms - last_heartbeat_ms) >= 2000) {
            last_heartbeat_ms = now_ms;
            ESP_LOGI(TAG, "USB_CLIENT_EVENTS_OBSERVE_DIAG heartbeat: loop=%d errors=%d", loop_count, error_count);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void usb_host_lib_events_only_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "USB_LIB_EVENTS_ONLY_DIAG: task started");
    ESP_LOGI(TAG, "USB_LIB_EVENTS_ONLY_DIAG: pumping usb_host_lib_handle_events only; no client events/callback/descriptor");

    int loop_count = 0;
    int64_t last_heartbeat_ms = 0;

    while (1) {
        loop_count++;
        uint32_t event_flags = 0;
        esp_err_t err = usb_host_lib_handle_events(pdMS_TO_TICKS(10), &event_flags);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "USB_LIB_EVENTS_ONLY_DIAG: lib event error: %s", esp_err_to_name(err));
        }
        if (event_flags != 0) {
            ESP_LOGI(TAG, "USB_LIB_EVENTS_ONLY_DIAG: event_flags=0x%lx", (unsigned long)event_flags);
        }

        int64_t now_ms = esp_timer_get_time() / 1000;
        if ((now_ms - last_heartbeat_ms) >= 2000) {
            last_heartbeat_ms = now_ms;
            ESP_LOGI(TAG, "USB_LIB_EVENTS_ONLY_DIAG heartbeat: loop=%d", loop_count);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void usb_host_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "📡 USB Host task started");
    ESP_LOGI(TAG, "STRICT_DISCOVERY: cooperative USB loop handles attach events and HID report descriptor only");

    int error_count = 0;
    const int MAX_ERRORS = 10;
    int loop_count = 0;
    int64_t last_heartbeat_ms = 0;

    while (1) {
        loop_count++;

        uint32_t event_flags = 0;
        esp_err_t lib_err = usb_host_lib_handle_events(pdMS_TO_TICKS(5), &event_flags);
        if (lib_err != ESP_OK && lib_err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "USB-LIB: event error: %s", esp_err_to_name(lib_err));
        }
        if (event_flags != 0) {
            ESP_LOGI(TAG, "USB-LIB: event_flags=0x%lx", (unsigned long)event_flags);
        }

        esp_err_t client_err = usb_host_client_handle_events(usb_client, pdMS_TO_TICKS(5));
        if (client_err != ESP_OK && client_err != ESP_ERR_TIMEOUT) {
            error_count++;
            ESP_LOGW(TAG, "USB-CLIENT: event error (%d/%d): %s",
                     error_count, MAX_ERRORS, esp_err_to_name(client_err));
            if (error_count >= MAX_ERRORS) {
                ESP_LOGE(TAG, "USB-CLIENT: too many event errors; stopping USB task");
                vTaskDelete(NULL);
                return;
            }
        } else if (client_err == ESP_OK) {
            error_count = 0;
            ESP_LOGI(TAG, "USB-CLIENT: event callback serviced");
        }

        if (ups_connected && ups_device != NULL && descriptor_needed && !descriptor_requested) {
            ESP_LOGI(TAG, "NUT-HID: submitting HID report descriptor request from USB task context");
            descriptor_requested = true;
            request_hid_report_descriptor(ups_device, HID_INTERFACE);
        }

        int64_t now_ms = esp_timer_get_time() / 1000;
        if ((now_ms - last_heartbeat_ms) >= 2000) {
            last_heartbeat_ms = now_ms;
            ESP_LOGI(TAG,
                     "STRICT_DISCOVERY heartbeat: loop=%d connected=%d needed=%d requested=%d complete=%d",
                     loop_count,
                     ups_connected,
                     descriptor_needed,
                     descriptor_requested,
                     descriptor_complete);
        }

        // Deliberately no telemetry polling and no MQTT reporting in this build.
        // After descriptor completion, remain idle so descriptor behavior is isolated.
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool usb_ups_is_connected(void)
{
    return ups_connected;
}

