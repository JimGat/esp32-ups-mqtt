#ifndef NUT_RUNTIME_MAP_H
#define NUT_RUNTIME_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hid_descriptor_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NUT_RUNTIME_KEY_UNKNOWN = 0,
    NUT_RUNTIME_KEY_BATTERY_CHARGE,
    NUT_RUNTIME_KEY_BATTERY_RUNTIME,
    NUT_RUNTIME_KEY_BATTERY_VOLTAGE,
    NUT_RUNTIME_KEY_INPUT_VOLTAGE,
    NUT_RUNTIME_KEY_LOAD_PERCENT,
    NUT_RUNTIME_KEY_STATUS_ACPRESENT,
    NUT_RUNTIME_KEY_STATUS_DISCHARGING,
    NUT_RUNTIME_KEY_STATUS_CHARGING,
    NUT_RUNTIME_KEY_STATUS_LOW_BATTERY,
    NUT_RUNTIME_KEY_STATUS_REPLACE_BATTERY,
    NUT_RUNTIME_KEY_STATUS_OVERLOAD,
} nut_runtime_key_t;

typedef enum {
    NUT_RUNTIME_SOURCE_DESCRIPTOR = 0,
    NUT_RUNTIME_SOURCE_QUIRK = 1,
    NUT_RUNTIME_SOURCE_DERIVED = 2,
} nut_runtime_source_t;

typedef enum {
    NUT_RUNTIME_CONF_DESCRIPTOR = 0,
    NUT_RUNTIME_CONF_QUIRK = 1,
    NUT_RUNTIME_CONF_DERIVED = 2,
    NUT_RUNTIME_CONF_UNMAPPED = 3,
} nut_runtime_confidence_t;

typedef enum {
    NUT_RUNTIME_STATUS_DESCRIPTOR_CONFIRMED = 0,
    NUT_RUNTIME_STATUS_DERIVED = 1,
    NUT_RUNTIME_STATUS_UNMAPPED = 2,
} nut_runtime_status_confidence_t;

typedef struct {
    nut_runtime_key_t key;
    char nut_name[40];
    char hid_path[96];
    hid_desc_report_type_t report_type;
    uint8_t report_id;
    uint16_t bit_offset;
    uint8_t bit_size;
    int32_t logical_min;
    int32_t logical_max;
    int8_t unit_exponent;
    nut_runtime_source_t source;
    nut_runtime_confidence_t confidence;
    const char *status_token;
    uint32_t last_raw_value;
    bool has_value;
} nut_runtime_map_entry_t;

size_t nut_runtime_map_build(const hid_descriptor_field_t *fields, size_t field_count,
                             nut_runtime_map_entry_t *entries, size_t max_entries);

const nut_runtime_map_entry_t *nut_runtime_map_find(const nut_runtime_map_entry_t *entries,
                                                    size_t entry_count,
                                                    nut_runtime_key_t key);

bool nut_runtime_map_compose_status(const nut_runtime_map_entry_t *entries, size_t entry_count,
                                    char *status, size_t status_size,
                                    nut_runtime_status_confidence_t *confidence);

const char *nut_runtime_key_str(nut_runtime_key_t key);
const char *nut_runtime_status_confidence_str(nut_runtime_status_confidence_t confidence);

size_t nut_runtime_map_entry_to_json(const nut_runtime_map_entry_t *entry, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* NUT_RUNTIME_MAP_H */
