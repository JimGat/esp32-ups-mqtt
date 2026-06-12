/*
 * ═══════════════════════════════════════════════════════════════════════════
 * UPS HID MAP - NUT-Style Descriptor-Backed Mapping Layer
 * ═══════════════════════════════════════════════════════════════════════════
 * PURPOSE:
 * v0.4 refactor: Replace guessed report-ID parsing with NUT-style HID path
 * mapping. This layer maps logical UPS usages (e.g. UPS.PowerSummary.RemainingCapacity)
 * to report IDs, bit offsets, sizes, units, and confidence levels.
 *
 * CONFIDENCE LEVELS:
 * - CONFIRMED: Descriptor-backed path validated against pull tests or NUT reference
 * - LIKELY: Working mapping based on pattern match, not yet descriptor-confirmed
 * - TENTATIVE: Guessed based on v0.3 manual exploration, await pull-test validation
 * - UNMAPPED: Not yet understood, do not publish
 *
 * REPORT TYPES:
 * - INPUT: Pushed by UPS via interrupt transfer (dynamic status)
 * - FEATURE: Requested by GET_REPORT (config-like or slow-changing)
 * - OUTPUT: Host → Device (not used for reading UPS status)
 *
 * ═══════════════════════════════════════════════════════════════════════════
 */

#ifndef UPS_HID_MAP_H
#define UPS_HID_MAP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Mapping Confidence Enum ---- */
typedef enum {
    MAP_CONFIRMED = 0,   /* Descriptor-backed, pull-test validated */
    MAP_LIKELY = 1,      /* Working pattern match, not yet descriptor-confirmed */
    MAP_TENTATIVE = 2,   /* v0.3 exploratory guess, await validation */
    MAP_UNMAPPED = 3,    /* Unknown, do not publish */
} ups_mapping_confidence_t;

/* ---- Report Type Enum ---- */
typedef enum {
    HID_REPORT_INPUT = 1,    /* Pushed by device (interrupt) */
    HID_REPORT_OUTPUT = 2,   /* Sent to device (not used) */
    HID_REPORT_FEATURE = 3,  /* Get/Set Feature Report (control transfer) */
} ups_hid_report_type_t;

/* ---- HID Mapping Struct ---- */
typedef struct {
    const char *nut_name;           /* NUT variable name, e.g. "battery.charge" */
    const char *hid_path;           /* Descriptor path, e.g. "UPS.PowerSummary.RemainingCapacity" */
    ups_hid_report_type_t type;     /* Input / Feature / Output */
    uint8_t report_id;              /* HID Report ID */
    uint16_t bit_offset;            /* Bit offset within report (0-based) */
    uint8_t bit_size;               /* Bit length (1-32) */
    int8_t unit_exponent;           /* Power-of-10 scale: value / 10^(-exponent) */
    float scale_override;           /* Multiplicative override (1.0 = none) */
    ups_mapping_confidence_t confidence; /* Confidence level */
    bool quick_poll;                /* Prioritize in feature report polls */
} ups_hid_mapping_t;

/* ---- Transport Health Counters ---- */
typedef struct {
    uint32_t usb_connected_count;      /* Number of times UPS connected */
    uint32_t usb_disconnected_count;   /* Number of times UPS disconnected */
    uint32_t poll_cycles_completed;    /* Complete poll cycles */
    uint32_t get_report_success;       /* Successful GET_REPORT calls */
    uint32_t get_report_timeout;       /* GET_REPORT timeouts */
    uint32_t get_report_stall;         /* GET_REPORT STALL errors */
    uint32_t get_report_error;         /* Other GET_REPORT errors */
    uint32_t last_usb_error;           /* Last error code (0 = none) */
    uint32_t last_poll_ms;             /* Timestamp of last complete poll (ms) */
    uint32_t max_poll_duration_ms;     /* Longest poll cycle (ms) */
    uint32_t snapshot_age_ms;          /* Age of current metrics snapshot (ms) */
} ups_transport_stats_t;

/* ---- Public Functions ---- */

/**
 * Get the static SMT2200 NUT-style mapping table.
 * Returns pointer to array and sets *count to number of entries.
 */
const ups_hid_mapping_t* ups_hid_map_get_table(size_t *count);

/**
 * Find a mapping by NUT variable name.
 * Returns NULL if not found.
 */
const ups_hid_mapping_t* ups_hid_map_lookup_nut_name(const char *nut_name);

/**
 * Render confidence level as human-readable string.
 */
const char* ups_mapping_confidence_str(ups_mapping_confidence_t conf);

/**
 * Render report type as human-readable string.
 */
const char* ups_hid_report_type_str(ups_hid_report_type_t type);

/**
 * Initialize transport stats (typically called once at boot).
 */
void ups_transport_stats_init(void);

/**
 * Get current transport stats.
 */
ups_transport_stats_t ups_transport_stats_get(void);

/**
 * Record a successful GET_REPORT.
 */
void ups_transport_stats_record_get_report_success(void);

/**
 * Record a GET_REPORT timeout.
 */
void ups_transport_stats_record_get_report_timeout(void);

/**
 * Record a GET_REPORT STALL.
 */
void ups_transport_stats_record_get_report_stall(void);

/**
 * Record a GET_REPORT error.
 */
void ups_transport_stats_record_get_report_error(uint32_t error_code);

/**
 * Record USB connection event.
 */
void ups_transport_stats_record_connected(void);

/**
 * Record USB disconnection event.
 */
void ups_transport_stats_record_disconnected(void);

/**
 * Record a complete poll cycle.
 */
void ups_transport_stats_record_poll_complete(uint32_t duration_ms);

/**
 * Update last poll timestamp and snapshot age.
 */
void ups_transport_stats_update_snapshot_age(uint32_t current_age_ms);

#ifdef __cplusplus
}
#endif

#endif /* UPS_HID_MAP_H */
