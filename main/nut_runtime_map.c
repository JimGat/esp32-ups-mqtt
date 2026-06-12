#include "nut_runtime_map.h"

#include <stdio.h>
#include <string.h>

static bool contains(const char *haystack, const char *needle)
{
    return haystack != NULL && needle != NULL && strstr(haystack, needle) != NULL;
}

static nut_runtime_key_t key_from_field(const hid_descriptor_field_t *field)
{
    if (field == NULL) return NUT_RUNTIME_KEY_UNKNOWN;

    if (strcmp(field->nut_name, "battery.charge") == 0) return NUT_RUNTIME_KEY_BATTERY_CHARGE;
    if (strcmp(field->nut_name, "battery.runtime") == 0) return NUT_RUNTIME_KEY_BATTERY_RUNTIME;
    if (strcmp(field->nut_name, "battery.voltage") == 0) return NUT_RUNTIME_KEY_BATTERY_VOLTAGE;
    if (strcmp(field->nut_name, "input.voltage") == 0) return NUT_RUNTIME_KEY_INPUT_VOLTAGE;
    if (strcmp(field->nut_name, "load.percent") == 0) return NUT_RUNTIME_KEY_LOAD_PERCENT;
    if (strcmp(field->nut_name, "ups.status.acpresent") == 0) return NUT_RUNTIME_KEY_STATUS_ACPRESENT;
    if (strcmp(field->nut_name, "ups.status.discharging") == 0) return NUT_RUNTIME_KEY_STATUS_DISCHARGING;
    if (strcmp(field->nut_name, "ups.status.charging") == 0) return NUT_RUNTIME_KEY_STATUS_CHARGING;
    if (strcmp(field->nut_name, "ups.status.low_battery") == 0) return NUT_RUNTIME_KEY_STATUS_LOW_BATTERY;
    if (strcmp(field->nut_name, "ups.status.replace_battery") == 0) return NUT_RUNTIME_KEY_STATUS_REPLACE_BATTERY;
    if (strcmp(field->nut_name, "ups.status.overload") == 0) return NUT_RUNTIME_KEY_STATUS_OVERLOAD;

    if (contains(field->hid_path, ".PresentStatus.ACPresent")) return NUT_RUNTIME_KEY_STATUS_ACPRESENT;
    if (contains(field->hid_path, ".PresentStatus.Discharging")) return NUT_RUNTIME_KEY_STATUS_DISCHARGING;
    if (contains(field->hid_path, ".PresentStatus.Charging")) return NUT_RUNTIME_KEY_STATUS_CHARGING;
    if (contains(field->hid_path, ".PresentStatus.BelowRemainingCapacityLimit")) return NUT_RUNTIME_KEY_STATUS_LOW_BATTERY;
    if (contains(field->hid_path, ".PresentStatus.NeedReplacement")) return NUT_RUNTIME_KEY_STATUS_REPLACE_BATTERY;
    if (contains(field->hid_path, ".PresentStatus.Overload")) return NUT_RUNTIME_KEY_STATUS_OVERLOAD;
    return NUT_RUNTIME_KEY_UNKNOWN;
}

static const char *status_token_for(nut_runtime_key_t key)
{
    switch (key) {
    case NUT_RUNTIME_KEY_STATUS_ACPRESENT: return "OL";
    case NUT_RUNTIME_KEY_STATUS_DISCHARGING: return "OB";
    case NUT_RUNTIME_KEY_STATUS_CHARGING: return "CHRG";
    case NUT_RUNTIME_KEY_STATUS_LOW_BATTERY: return "LB";
    case NUT_RUNTIME_KEY_STATUS_REPLACE_BATTERY: return "RB";
    case NUT_RUNTIME_KEY_STATUS_OVERLOAD: return "OVER";
    default: return "";
    }
}

static bool is_status_key(nut_runtime_key_t key)
{
    return key >= NUT_RUNTIME_KEY_STATUS_ACPRESENT && key <= NUT_RUNTIME_KEY_STATUS_OVERLOAD;
}

size_t nut_runtime_map_build(const hid_descriptor_field_t *fields, size_t field_count,
                             nut_runtime_map_entry_t *entries, size_t max_entries)
{
    if (fields == NULL || entries == NULL || max_entries == 0) return 0;

    size_t out = 0;
    for (size_t i = 0; i < field_count && out < max_entries; i++) {
        nut_runtime_key_t key = key_from_field(&fields[i]);
        if (key == NUT_RUNTIME_KEY_UNKNOWN) continue;

        nut_runtime_map_entry_t *entry = &entries[out++];
        memset(entry, 0, sizeof(*entry));
        entry->key = key;
        snprintf(entry->nut_name, sizeof(entry->nut_name), "%s", fields[i].nut_name);
        snprintf(entry->hid_path, sizeof(entry->hid_path), "%s", fields[i].hid_path);
        entry->report_type = fields[i].type;
        entry->report_id = fields[i].report_id;
        entry->bit_offset = fields[i].bit_offset;
        entry->bit_size = fields[i].bit_size;
        entry->logical_min = fields[i].logical_min;
        entry->logical_max = fields[i].logical_max;
        entry->unit_exponent = fields[i].unit_exponent;
        entry->source = NUT_RUNTIME_SOURCE_DESCRIPTOR;
        entry->confidence = NUT_RUNTIME_CONF_DESCRIPTOR;
        entry->status_token = status_token_for(key);
    }
    return out;
}

const nut_runtime_map_entry_t *nut_runtime_map_find(const nut_runtime_map_entry_t *entries,
                                                    size_t entry_count,
                                                    nut_runtime_key_t key)
{
    if (entries == NULL) return NULL;
    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].key == key) return &entries[i];
    }
    return NULL;
}

bool nut_runtime_map_compose_status(const nut_runtime_map_entry_t *entries, size_t entry_count,
                                    char *status, size_t status_size,
                                    nut_runtime_status_confidence_t *confidence)
{
    if (status == NULL || status_size == 0) return false;
    status[0] = '\0';
    bool wrote = false;

    if (entries != NULL) {
        for (size_t i = 0; i < entry_count; i++) {
            if (!is_status_key(entries[i].key)) continue;
            if (entries[i].confidence != NUT_RUNTIME_CONF_DESCRIPTOR) continue;
            if (!entries[i].has_value || entries[i].last_raw_value == 0) continue;
            const char *token = entries[i].status_token ? entries[i].status_token : "";
            if (token[0] == '\0') continue;
            if (wrote) strncat(status, " ", status_size - strlen(status) - 1);
            strncat(status, token, status_size - strlen(status) - 1);
            wrote = true;
        }
    }

    if (!wrote) {
        snprintf(status, status_size, "UNKNOWN");
        if (confidence) *confidence = NUT_RUNTIME_STATUS_UNMAPPED;
        return false;
    }

    if (confidence) *confidence = NUT_RUNTIME_STATUS_DESCRIPTOR_CONFIRMED;
    return true;
}

const char *nut_runtime_key_str(nut_runtime_key_t key)
{
    switch (key) {
    case NUT_RUNTIME_KEY_BATTERY_CHARGE: return "battery.charge";
    case NUT_RUNTIME_KEY_BATTERY_RUNTIME: return "battery.runtime";
    case NUT_RUNTIME_KEY_BATTERY_VOLTAGE: return "battery.voltage";
    case NUT_RUNTIME_KEY_INPUT_VOLTAGE: return "input.voltage";
    case NUT_RUNTIME_KEY_LOAD_PERCENT: return "load.percent";
    case NUT_RUNTIME_KEY_STATUS_ACPRESENT: return "ups.status.acpresent";
    case NUT_RUNTIME_KEY_STATUS_DISCHARGING: return "ups.status.discharging";
    case NUT_RUNTIME_KEY_STATUS_CHARGING: return "ups.status.charging";
    case NUT_RUNTIME_KEY_STATUS_LOW_BATTERY: return "ups.status.low_battery";
    case NUT_RUNTIME_KEY_STATUS_REPLACE_BATTERY: return "ups.status.replace_battery";
    case NUT_RUNTIME_KEY_STATUS_OVERLOAD: return "ups.status.overload";
    case NUT_RUNTIME_KEY_UNKNOWN:
    default: return "unknown";
    }
}

const char *nut_runtime_status_confidence_str(nut_runtime_status_confidence_t confidence)
{
    switch (confidence) {
    case NUT_RUNTIME_STATUS_DESCRIPTOR_CONFIRMED: return "descriptor_confirmed";
    case NUT_RUNTIME_STATUS_DERIVED: return "derived";
    case NUT_RUNTIME_STATUS_UNMAPPED:
    default: return "unmapped";
    }
}
