#include <assert.h>
#include <string.h>

#include "hid_descriptor_parser.h"
#include "nut_runtime_map.h"

static hid_descriptor_field_t field(hid_desc_report_type_t type, uint8_t report_id,
                                    uint16_t bit_offset, uint8_t bit_size,
                                    const char *path, const char *nut_name)
{
    hid_descriptor_field_t f = {0};
    f.type = type;
    f.report_id = report_id;
    f.bit_offset = bit_offset;
    f.bit_size = bit_size;
    f.logical_min = 0;
    f.logical_max = 1;
    f.unit_exponent = 0;
    strncpy(f.hid_path, path, sizeof(f.hid_path) - 1);
    strncpy(f.nut_name, nut_name, sizeof(f.nut_name) - 1);
    return f;
}

static void test_builds_status_sources_from_descriptor_semantics(void)
{
    hid_descriptor_field_t fields[] = {
        field(HID_DESC_REPORT_INPUT, 0x21, 8, 1,
              "UPS.PowerSummary.PresentStatus.ACPresent", "ups.status.acpresent"),
        field(HID_DESC_REPORT_INPUT, 0x21, 9, 1,
              "UPS.PowerSummary.PresentStatus.Discharging", "ups.status.discharging"),
        field(HID_DESC_REPORT_FEATURE, 0x0C, 8, 8,
              "UPS.PowerSummary.RemainingCapacity", "battery.charge"),
    };

    nut_runtime_map_entry_t entries[8];
    size_t count = nut_runtime_map_build(fields, 3, entries, 8);

    assert(count == 3);
    const nut_runtime_map_entry_t *ac = nut_runtime_map_find(entries, count, NUT_RUNTIME_KEY_STATUS_ACPRESENT);
    const nut_runtime_map_entry_t *ob = nut_runtime_map_find(entries, count, NUT_RUNTIME_KEY_STATUS_DISCHARGING);
    const nut_runtime_map_entry_t *charge = nut_runtime_map_find(entries, count, NUT_RUNTIME_KEY_BATTERY_CHARGE);

    assert(ac != NULL);
    assert(ac->source == NUT_RUNTIME_SOURCE_DESCRIPTOR);
    assert(ac->confidence == NUT_RUNTIME_CONF_DESCRIPTOR);
    assert(ac->report_type == HID_DESC_REPORT_INPUT);
    assert(ac->report_id == 0x21);
    assert(ac->bit_offset == 8);
    assert(strcmp(ac->status_token, "OL") == 0);

    assert(ob != NULL);
    assert(strcmp(ob->status_token, "OB") == 0);

    assert(charge != NULL);
    assert(charge->report_type == HID_DESC_REPORT_FEATURE);
    assert(charge->report_id == 0x0C);
}

static void test_composes_status_only_from_confirmed_descriptor_flags(void)
{
    nut_runtime_map_entry_t entries[] = {
        {
            .key = NUT_RUNTIME_KEY_STATUS_ACPRESENT,
            .confidence = NUT_RUNTIME_CONF_DESCRIPTOR,
            .status_token = "OL",
            .last_raw_value = 1,
            .has_value = true,
        },
        {
            .key = NUT_RUNTIME_KEY_STATUS_DISCHARGING,
            .confidence = NUT_RUNTIME_CONF_DESCRIPTOR,
            .status_token = "OB",
            .last_raw_value = 0,
            .has_value = true,
        },
    };
    char status[32];
    nut_runtime_status_confidence_t conf = NUT_RUNTIME_STATUS_UNMAPPED;

    bool ok = nut_runtime_map_compose_status(entries, 2, status, sizeof(status), &conf);

    assert(ok == true);
    assert(strcmp(status, "OL") == 0);
    assert(conf == NUT_RUNTIME_STATUS_DESCRIPTOR_CONFIRMED);
}

static void test_unknown_status_without_descriptor_status_sources(void)
{
    hid_descriptor_field_t fields[] = {
        field(HID_DESC_REPORT_FEATURE, 0x0D, 8, 16,
              "UPS.Input.Voltage", "input.voltage"),
    };
    nut_runtime_map_entry_t entries[4];
    size_t count = nut_runtime_map_build(fields, 1, entries, 4);
    char status[32];
    nut_runtime_status_confidence_t conf = NUT_RUNTIME_STATUS_DESCRIPTOR_CONFIRMED;

    bool ok = nut_runtime_map_compose_status(entries, count, status, sizeof(status), &conf);

    assert(ok == false);
    assert(strcmp(status, "UNKNOWN") == 0);
    assert(conf == NUT_RUNTIME_STATUS_UNMAPPED);
}

int main(void)
{
    test_builds_status_sources_from_descriptor_semantics();
    test_composes_status_only_from_confirmed_descriptor_flags();
    test_unknown_status_without_descriptor_status_sources();
    return 0;
}
