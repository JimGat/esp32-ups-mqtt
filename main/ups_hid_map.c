/*
 * ═══════════════════════════════════════════════════════════════════════════
 * UPS HID MAP - NUT-Style Descriptor-Backed Mapping Layer (Implementation)
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include "ups_hid_map.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "ups_hid_map";

/* ---- Static SMT2200 / APC 5G Family Mapping Table ---- */
static const ups_hid_mapping_t smt2200_mapping_table[] = {
    /* Battery Metrics */
    {
        .nut_name = "battery.charge",
        .hid_path = "UPS.PowerSummary.RemainingCapacity",
        .type = HID_REPORT_INPUT,
        .report_id = 0x0C,
        .bit_offset = 8,
        .bit_size = 8,
        .unit_exponent = 0,
        .scale_override = 1.0f,
        .confidence = MAP_LIKELY,  /* Working reliably, not yet descriptor-confirmed */
        .quick_poll = true,
    },
    {
        .nut_name = "battery.runtime",
        .hid_path = "UPS.PowerSummary.RunTimeToEmpty",
        .type = HID_REPORT_INPUT,
        .report_id = 0x0C,
        .bit_offset = 16,
        .bit_size = 16,
        .unit_exponent = 0,
        .scale_override = 1.0f,
        .confidence = MAP_TENTATIVE,  /* Appears in 4+ byte variant, verify live updates */
        .quick_poll = false,
    },
    {
        .nut_name = "battery.voltage",
        .hid_path = "UPS.Battery.Voltage",
        .type = HID_REPORT_FEATURE,
        .report_id = 0x0B,
        .bit_offset = 8,
        .bit_size = 16,
        .unit_exponent = -2,  /* value / 100 */
        .scale_override = 1.0f,
        .confidence = MAP_LIKELY,  /* Pattern matches observed data */
        .quick_poll = true,
    },
    {
        .nut_name = "input.voltage",
        .hid_path = "UPS.Input.Voltage",
        .type = HID_REPORT_FEATURE,
        .report_id = 0x0D,
        .bit_offset = 8,
        .bit_size = 16,
        .unit_exponent = -1,  /* value / 10 */
        .scale_override = 1.0f,
        .confidence = MAP_LIKELY,  /* Pattern matches observed data */
        .quick_poll = true,
    },
    {
        .nut_name = "load.percent",
        .hid_path = "UPS.PowerConverter.PercentLoad",
        .type = HID_REPORT_FEATURE,
        .report_id = 0x08,
        .bit_offset = 8,
        .bit_size = 16,
        .unit_exponent = -1,  /* value / 10 */
        .scale_override = 1.0f,
        .confidence = MAP_LIKELY,  /* Working for SMT2200, not descriptor-confirmed */
        .quick_poll = true,
    },

    /* Status/Line Fail - CRITICAL: These remain UNMAPPED until descriptor or pull-test confirms */
    {
        .nut_name = "ups.status",
        .hid_path = "UPS.PowerSummary.PresentStatus.ACPresent",
        .type = HID_REPORT_FEATURE,
        .report_id = 0x09,
        .bit_offset = 8,
        .bit_size = 16,
        .unit_exponent = 0,
        .scale_override = 1.0f,
        .confidence = MAP_UNMAPPED,  /* v0.3 bit guesses failed pull tests; await descriptor or NUT confirmation */
        .quick_poll = true,
    },
    {
        .nut_name = "input.transfer.reason",
        .hid_path = "UPS.Input.APCLineFailCause",
        .type = HID_REPORT_FEATURE,
        .report_id = 0x09,  /* May be different report; TBD by descriptor */
        .bit_offset = 0,
        .bit_size = 16,
        .unit_exponent = 0,
        .scale_override = 1.0f,
        .confidence = MAP_UNMAPPED,  /* Descriptor or NUT reference required */
        .quick_poll = true,
    },

    /* Config/Threshold Metrics */
    {
        .nut_name = "battery.nominal.voltage",
        .hid_path = "UPS.PowerSummary.ConfigVoltage",
        .type = HID_REPORT_FEATURE,
        .report_id = 0x30,
        .bit_offset = 8,
        .bit_size = 8,
        .unit_exponent = 0,
        .scale_override = 1.0f,
        .confidence = MAP_TENTATIVE,
        .quick_poll = false,
    },
    {
        .nut_name = "input.transfer.low",
        .hid_path = "UPS.Input.LowVoltageTransfer",
        .type = HID_REPORT_FEATURE,
        .report_id = 0x32,
        .bit_offset = 8,
        .bit_size = 16,
        .unit_exponent = -2,
        .scale_override = 1.0f,
        .confidence = MAP_TENTATIVE,
        .quick_poll = false,
    },
    {
        .nut_name = "input.transfer.high",
        .hid_path = "UPS.Input.HighVoltageTransfer",
        .type = HID_REPORT_FEATURE,
        .report_id = 0x33,
        .bit_offset = 8,
        .bit_size = 16,
        .unit_exponent = -2,
        .scale_override = 1.0f,
        .confidence = MAP_TENTATIVE,
        .quick_poll = false,
    },
    {
        .nut_name = "battery.charge.low",
        .hid_path = "UPS.PowerSummary.RemainingCapacityLimit",
        .type = HID_REPORT_FEATURE,
        .report_id = 0x11,
        .bit_offset = 8,
        .bit_size = 8,
        .unit_exponent = 0,
        .scale_override = 1.0f,
        .confidence = MAP_TENTATIVE,
        .quick_poll = false,
    },
};

static const size_t smt2200_mapping_table_size = sizeof(smt2200_mapping_table) / sizeof(smt2200_mapping_table[0]);

/* ---- Transport Stats (Global, Protected by Task Safety) ---- */
static ups_transport_stats_t transport_stats = {0};
static SemaphoreHandle_t transport_stats_mutex = NULL;

/* ---- Initialization ---- */
void ups_transport_stats_init(void)
{
    if (transport_stats_mutex == NULL) {
        transport_stats_mutex = xSemaphoreCreateMutex();
    }
    memset(&transport_stats, 0, sizeof(transport_stats));
    transport_stats.last_poll_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
}

/* ---- Mapping Table Access ---- */
const ups_hid_mapping_t* ups_hid_map_get_table(size_t *count)
{
    if (count != NULL) {
        *count = smt2200_mapping_table_size;
    }
    return smt2200_mapping_table;
}

const ups_hid_mapping_t* ups_hid_map_lookup_nut_name(const char *nut_name)
{
    if (nut_name == NULL) return NULL;

    for (size_t i = 0; i < smt2200_mapping_table_size; i++) {
        if (strcmp(smt2200_mapping_table[i].nut_name, nut_name) == 0) {
            return &smt2200_mapping_table[i];
        }
    }
    return NULL;
}

/* ---- String Rendering ---- */
const char* ups_mapping_confidence_str(ups_mapping_confidence_t conf)
{
    switch (conf) {
        case MAP_CONFIRMED:  return "confirmed";
        case MAP_LIKELY:     return "likely";
        case MAP_TENTATIVE:  return "tentative";
        case MAP_UNMAPPED:   return "unmapped";
        default:             return "unknown";
    }
}

const char* ups_hid_report_type_str(ups_hid_report_type_t type)
{
    switch (type) {
        case HID_REPORT_INPUT:   return "input";
        case HID_REPORT_OUTPUT:  return "output";
        case HID_REPORT_FEATURE: return "feature";
        default:                 return "unknown";
    }
}

/* ---- Transport Stats Accessors ---- */
ups_transport_stats_t ups_transport_stats_get(void)
{
    ups_transport_stats_t stats;
    if (transport_stats_mutex != NULL) {
        xSemaphoreTake(transport_stats_mutex, pdMS_TO_TICKS(100));
    }
    memcpy(&stats, &transport_stats, sizeof(ups_transport_stats_t));
    if (transport_stats_mutex != NULL) {
        xSemaphoreGive(transport_stats_mutex);
    }
    return stats;
}

static void _stats_lock(void)
{
    if (transport_stats_mutex != NULL) {
        xSemaphoreTake(transport_stats_mutex, portMAX_DELAY);
    }
}

static void _stats_unlock(void)
{
    if (transport_stats_mutex != NULL) {
        xSemaphoreGive(transport_stats_mutex);
    }
}

void ups_transport_stats_record_get_report_success(void)
{
    _stats_lock();
    transport_stats.get_report_success++;
    transport_stats.last_usb_error = 0;
    _stats_unlock();
}

void ups_transport_stats_record_get_report_timeout(void)
{
    _stats_lock();
    transport_stats.get_report_timeout++;
    transport_stats.last_usb_error = 0x01;  /* Timeout marker */
    _stats_unlock();
}

void ups_transport_stats_record_get_report_stall(void)
{
    _stats_lock();
    transport_stats.get_report_stall++;
    transport_stats.last_usb_error = 0x02;  /* STALL marker */
    _stats_unlock();
}

void ups_transport_stats_record_get_report_error(uint32_t error_code)
{
    _stats_lock();
    transport_stats.get_report_error++;
    transport_stats.last_usb_error = error_code;
    _stats_unlock();
}

void ups_transport_stats_record_connected(void)
{
    _stats_lock();
    transport_stats.usb_connected_count++;
    transport_stats.last_usb_error = 0;
    _stats_unlock();
}

void ups_transport_stats_record_disconnected(void)
{
    _stats_lock();
    transport_stats.usb_disconnected_count++;
    _stats_unlock();
}

void ups_transport_stats_record_poll_complete(uint32_t duration_ms)
{
    _stats_lock();
    transport_stats.poll_cycles_completed++;
    transport_stats.last_poll_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (duration_ms > transport_stats.max_poll_duration_ms) {
        transport_stats.max_poll_duration_ms = duration_ms;
    }
    _stats_unlock();
}

void ups_transport_stats_update_snapshot_age(uint32_t current_age_ms)
{
    _stats_lock();
    transport_stats.snapshot_age_ms = current_age_ms;
    _stats_unlock();
}
