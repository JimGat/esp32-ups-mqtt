#include "hid_descriptor_parser.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    uint16_t page;
    uint16_t id;
} usage_ref_t;

typedef struct {
    uint16_t usage_page;
    uint8_t report_id;
    uint8_t report_size;
    uint8_t report_count;
    int32_t logical_min;
    int32_t logical_max;
    int8_t unit_exponent;
} hid_globals_t;

typedef struct {
    usage_ref_t usages[24];
    uint8_t usage_count;
    bool has_range;
    usage_ref_t usage_min;
    usage_ref_t usage_max;
} hid_locals_t;

typedef struct {
    usage_ref_t stack[8];
    uint8_t depth;
} collection_stack_t;

static void clear_locals(hid_locals_t *locals)
{
    memset(locals, 0, sizeof(*locals));
}

static int32_t sign_extend(uint32_t value, uint8_t size)
{
    if (size == 0 || size >= 4) {
        return (int32_t)value;
    }
    uint32_t sign_bit = 1u << (size * 8 - 1);
    uint32_t mask = (1u << (size * 8)) - 1u;
    value &= mask;
    if ((value & sign_bit) != 0) {
        value |= ~mask;
    }
    return (int32_t)value;
}

static uint32_t read_le(const uint8_t *data, uint8_t size)
{
    uint32_t value = 0;
    for (uint8_t i = 0; i < size; i++) {
        value |= ((uint32_t)data[i]) << (8 * i);
    }
    return value;
}

const char *hid_descriptor_report_type_str(hid_desc_report_type_t type)
{
    switch (type) {
    case HID_DESC_REPORT_INPUT:
        return "Input";
    case HID_DESC_REPORT_OUTPUT:
        return "Output";
    case HID_DESC_REPORT_FEATURE:
        return "Feature";
    default:
        return "Unknown";
    }
}

const char *hid_descriptor_usage_name(uint16_t page, uint16_t id)
{
    if (page == 0x84) {
        switch (id) {
        case 0x04: return "UPS";
        case 0x10: return "BatterySystem";
        case 0x12: return "Battery";
        case 0x1A: return "Input";
        case 0x1C: return "Output";
        case 0x1D: return "Flow";
        case 0x24: return "PowerSummary";
        case 0x2C: return "PowerConverter";
        case 0x30: return "Voltage";
        case 0x35: return "RemainingCapacity";
        case 0x36: return "RunTimeToEmpty";
        case 0x40: return "ConfigVoltage";
        case 0x44: return "LowVoltageTransfer";
        case 0x45: return "HighVoltageTransfer";
        case 0x53: return "ACPresent";
        case 0x56: return "BelowRemainingCapacityLimit";
        case 0x57: return "Charging";
        case 0x58: return "Discharging";
        case 0x5A: return "NeedReplacement";
        case 0x65: return "PresentStatus";
        case 0x66: return "RemainingCapacity";
        case 0x68: return "RunTimeToEmpty";
        case 0x6B: return "Overload";
        case 0x83: return "APCLineFailCause";
        default: break;
        }
    }
    if (page == 0x85) {
        switch (id) {
        case 0x66: return "RemainingCapacity";
        case 0x68: return "RunTimeToEmpty";
        default: break;
        }
    }
    return NULL;
}

static bool is_status_usage(uint16_t page, uint16_t id)
{
    return page == 0x84 &&
           (id == 0x53 || id == 0x56 || id == 0x57 || id == 0x58 ||
            id == 0x5A || id == 0x6B);
}

static bool stack_contains(const collection_stack_t *stack, const char *name)
{
    for (uint8_t i = 0; i < stack->depth; i++) {
        const char *candidate = hid_descriptor_usage_name(stack->stack[i].page, stack->stack[i].id);
        if (candidate != NULL && strcmp(candidate, name) == 0) {
            return true;
        }
    }
    return false;
}

static void build_path(const collection_stack_t *stack, usage_ref_t usage, char *out, size_t out_size)
{
    char prefix[256] = {0};
    bool have_ups = false;

    for (uint8_t i = 0; i < stack->depth; i++) {
        const char *name = hid_descriptor_usage_name(stack->stack[i].page, stack->stack[i].id);
        if (name == NULL) {
            continue;
        }
        if (strcmp(name, "UPS") == 0) {
            have_ups = true;
        }
        if (prefix[0] != '\0') {
            strncat(prefix, ".", sizeof(prefix) - strlen(prefix) - 1);
        }
        strncat(prefix, name, sizeof(prefix) - strlen(prefix) - 1);
    }

    if (!have_ups) {
        if (prefix[0] != '\0') {
            size_t len = strlen(prefix);
            if (len + 4 < sizeof(prefix)) {
                memmove(prefix + 4, prefix, len + 1);
                memcpy(prefix, "UPS.", 4);
            } else {
                strncpy(prefix, "UPS", sizeof(prefix) - 1);
            }
        } else {
            strncpy(prefix, "UPS", sizeof(prefix) - 1);
        }
    }

    const char *usage_name = hid_descriptor_usage_name(usage.page, usage.id);
    char usage_buf[32];
    if (usage_name == NULL) {
        snprintf(usage_buf, sizeof(usage_buf), "Usage%04X_%04X", usage.page, usage.id);
        usage_name = usage_buf;
    }

    char path[320] = {0};
    strncat(path, prefix, sizeof(path) - strlen(path) - 1);
    if (is_status_usage(usage.page, usage.id) && !stack_contains(stack, "PresentStatus")) {
        strncat(path, ".PresentStatus", sizeof(path) - strlen(path) - 1);
    }
    strncat(path, ".", sizeof(path) - strlen(path) - 1);
    strncat(path, usage_name, sizeof(path) - strlen(path) - 1);

    if (out_size > 0) {
        strncpy(out, path, out_size - 1);
        out[out_size - 1] = '\0';
    }
}

static void map_nut_name(const char *path, uint16_t page, uint16_t id, char *out, size_t out_size)
{
    const char *name = "";
    if (strstr(path, ".PresentStatus.ACPresent") != NULL || (page == 0x84 && id == 0x53)) {
        name = "ups.status.acpresent";
    } else if (strstr(path, ".PresentStatus.Charging") != NULL || (page == 0x84 && id == 0x57)) {
        name = "ups.status.charging";
    } else if (strstr(path, ".PresentStatus.Discharging") != NULL || (page == 0x84 && id == 0x58)) {
        name = "ups.status.discharging";
    } else if (strstr(path, ".PresentStatus.BelowRemainingCapacityLimit") != NULL || (page == 0x84 && id == 0x56)) {
        name = "ups.status.low_battery";
    } else if (strstr(path, ".PresentStatus.Overload") != NULL || (page == 0x84 && id == 0x6B)) {
        name = "ups.status.overload";
    } else if (strstr(path, ".PresentStatus.NeedReplacement") != NULL || (page == 0x84 && id == 0x5A)) {
        name = "ups.status.replace_battery";
    } else if (strstr(path, ".APCLineFailCause") != NULL || (page == 0x84 && id == 0x83)) {
        name = "input.transfer.reason";
    } else if (strstr(path, ".RemainingCapacity") != NULL || (page == 0x84 && (id == 0x66 || id == 0x35))) {
        name = "battery.charge";
    } else if (strstr(path, ".RunTimeToEmpty") != NULL || (page == 0x84 && (id == 0x68 || id == 0x36))) {
        name = "battery.runtime";
    } else if (strstr(path, ".Input.Voltage") != NULL || (page == 0x84 && id == 0x30)) {
        name = "input.voltage";
    } else if (strstr(path, ".Battery.Voltage") != NULL || strstr(path, ".PowerSummary.Voltage") != NULL || (page == 0x84 && id == 0x4C)) {
        name = "battery.voltage";
    } else if (strstr(path, ".PercentLoad") != NULL || (page == 0x84 && id == 0x6F)) {
        name = "load.percent";
    }
    snprintf(out, out_size, "%s", name);
}

static uint16_t *offset_table_for(hid_desc_report_type_t type,
                                  uint16_t input_offsets[256],
                                  uint16_t output_offsets[256],
                                  uint16_t feature_offsets[256])
{
    switch (type) {
    case HID_DESC_REPORT_INPUT: return input_offsets;
    case HID_DESC_REPORT_OUTPUT: return output_offsets;
    case HID_DESC_REPORT_FEATURE: return feature_offsets;
    default: return input_offsets;
    }
}

static void add_main_fields(hid_desc_report_type_t type, uint8_t main_flags,
                            const hid_globals_t *globals, const hid_locals_t *locals,
                            const collection_stack_t *stack,
                            hid_descriptor_field_t *fields, size_t max_fields,
                            size_t *field_count,
                            uint16_t input_offsets[256], uint16_t output_offsets[256],
                            uint16_t feature_offsets[256])
{
    uint16_t *offsets = offset_table_for(type, input_offsets, output_offsets, feature_offsets);
    uint8_t report_id = globals->report_id;
    if (report_id != 0 && offsets[report_id] == 0) {
        offsets[report_id] = 8;
    }

    bool constant = (main_flags & 0x01) != 0;
    for (uint8_t i = 0; i < globals->report_count; i++) {
        usage_ref_t usage = { globals->usage_page, 0 };
        if (i < locals->usage_count) {
            usage = locals->usages[i];
        } else if (locals->has_range && locals->usage_min.page == locals->usage_max.page &&
                   locals->usage_min.id + i <= locals->usage_max.id) {
            usage.page = locals->usage_min.page;
            usage.id = (uint16_t)(locals->usage_min.id + i);
        }

        if (!constant && usage.id != 0 && *field_count < max_fields) {
            hid_descriptor_field_t *field = &fields[*field_count];
            memset(field, 0, sizeof(*field));
            field->type = type;
            field->report_id = report_id;
            field->usage_page = usage.page;
            field->usage_id = usage.id;
            field->bit_offset = offsets[report_id];
            field->bit_size = globals->report_size;
            field->logical_min = globals->logical_min;
            field->logical_max = globals->logical_max;
            field->unit_exponent = globals->unit_exponent;
            build_path(stack, usage, field->hid_path, sizeof(field->hid_path));
            map_nut_name(field->hid_path, field->usage_page, field->usage_id, field->nut_name, sizeof(field->nut_name));
            (*field_count)++;
        }
        offsets[report_id] = (uint16_t)(offsets[report_id] + globals->report_size);
    }
}

bool hid_descriptor_parse(const uint8_t *descriptor, size_t descriptor_len,
                          hid_descriptor_field_t *fields, size_t max_fields,
                          size_t *field_count)
{
    if (descriptor == NULL || fields == NULL || field_count == NULL) {
        return false;
    }

    *field_count = 0;
    hid_globals_t globals = {0};
    hid_locals_t locals;
    collection_stack_t stack = {0};
    uint16_t input_offsets[256] = {0};
    uint16_t output_offsets[256] = {0};
    uint16_t feature_offsets[256] = {0};
    clear_locals(&locals);

    size_t pos = 0;
    while (pos < descriptor_len) {
        uint8_t prefix = descriptor[pos++];
        if (prefix == 0xFE) {
            if (pos + 2 > descriptor_len) return false;
            uint8_t long_size = descriptor[pos++];
            pos++;
            if (pos + long_size > descriptor_len) return false;
            pos += long_size;
            continue;
        }

        uint8_t size_code = prefix & 0x03;
        uint8_t size = (size_code == 3) ? 4 : size_code;
        uint8_t type = (prefix >> 2) & 0x03;
        uint8_t tag = (prefix >> 4) & 0x0F;
        if (pos + size > descriptor_len) {
            return false;
        }

        uint32_t raw = read_le(&descriptor[pos], size);
        int32_t signed_value = sign_extend(raw, size);
        pos += size;

        if (type == 0) {
            if (tag == 0x08) {
                add_main_fields(HID_DESC_REPORT_INPUT, raw & 0xFF, &globals, &locals, &stack,
                                fields, max_fields, field_count, input_offsets, output_offsets, feature_offsets);
            } else if (tag == 0x09) {
                add_main_fields(HID_DESC_REPORT_OUTPUT, raw & 0xFF, &globals, &locals, &stack,
                                fields, max_fields, field_count, input_offsets, output_offsets, feature_offsets);
            } else if (tag == 0x0B) {
                add_main_fields(HID_DESC_REPORT_FEATURE, raw & 0xFF, &globals, &locals, &stack,
                                fields, max_fields, field_count, input_offsets, output_offsets, feature_offsets);
            } else if (tag == 0x0A) {
                if (locals.usage_count > 0 && stack.depth < sizeof(stack.stack) / sizeof(stack.stack[0])) {
                    stack.stack[stack.depth++] = locals.usages[0];
                }
            } else if (tag == 0x0C) {
                if (stack.depth > 0) {
                    stack.depth--;
                }
            }
            clear_locals(&locals);
        } else if (type == 1) {
            switch (tag) {
            case 0x00: globals.usage_page = raw & 0xFFFF; break;
            case 0x01: globals.logical_min = signed_value; break;
            case 0x02: globals.logical_max = signed_value; break;
            case 0x05: globals.unit_exponent = (int8_t)signed_value; break;
            case 0x07: globals.report_size = raw & 0xFF; break;
            case 0x08: globals.report_id = raw & 0xFF; break;
            case 0x09: globals.report_count = raw & 0xFF; break;
            default: break;
            }
        } else if (type == 2) {
            usage_ref_t usage = { globals.usage_page, raw & 0xFFFF };
            if ((raw >> 16) != 0) {
                usage.page = (raw >> 16) & 0xFFFF;
                usage.id = raw & 0xFFFF;
            }
            if (tag == 0x00) {
                if (locals.usage_count < sizeof(locals.usages) / sizeof(locals.usages[0])) {
                    locals.usages[locals.usage_count++] = usage;
                }
            } else if (tag == 0x01) {
                locals.usage_min = usage;
                locals.has_range = true;
            } else if (tag == 0x02) {
                locals.usage_max = usage;
                locals.has_range = true;
            }
        }
    }
    return true;
}

const hid_descriptor_field_t *hid_descriptor_find_path(const hid_descriptor_field_t *fields,
                                                       size_t count,
                                                       const char *suffix)
{
    if (fields == NULL || suffix == NULL) {
        return NULL;
    }
    size_t suffix_len = strlen(suffix);
    for (size_t i = 0; i < count; i++) {
        size_t path_len = strlen(fields[i].hid_path);
        if (path_len >= suffix_len && strcmp(fields[i].hid_path + path_len - suffix_len, suffix) == 0) {
            return &fields[i];
        }
    }
    return NULL;
}
