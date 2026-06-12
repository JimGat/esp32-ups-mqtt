#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "hid_descriptor_parser.h"

static void test_simple_power_descriptor_paths(void)
{
    const uint8_t descriptor[] = {
        0x05, 0x84,       /* Usage Page (Power Device) */
        0x09, 0x04,       /* Usage (UPS) */
        0xA1, 0x01,       /* Collection (Application) */
        0x09, 0x24,       /* Usage (PowerSummary) */
        0xA1, 0x02,       /* Collection (Logical) */
        0x85, 0x09,       /* Report ID 9 */
        0x75, 0x01,       /* Report Size 1 */
        0x95, 0x03,       /* Report Count 3 */
        0x09, 0x53,       /* Usage (ACPresent) */
        0x09, 0x57,       /* Usage (Charging) */
        0x09, 0x58,       /* Usage (Discharging) */
        0x81, 0x02,       /* Input (Data,Var,Abs) */
        0x85, 0x0C,       /* Report ID 12 */
        0x75, 0x08,       /* Report Size 8 */
        0x95, 0x01,       /* Report Count 1 */
        0x09, 0x66,       /* Usage (RemainingCapacity) */
        0xB1, 0x02,       /* Feature (Data,Var,Abs) */
        0xC0, 0xC0
    };
    hid_descriptor_field_t fields[8];
    size_t count = 0;
    assert(hid_descriptor_parse(descriptor, sizeof(descriptor), fields, 8, &count) == true);
    assert(count == 4);
    assert(fields[0].report_id == 0x09);
    assert(fields[0].type == HID_DESC_REPORT_INPUT);
    assert(fields[0].bit_offset == 8);
    assert(fields[0].bit_size == 1);
    assert(strcmp(fields[0].hid_path, "UPS.PowerSummary.PresentStatus.ACPresent") == 0);
    assert(strcmp(fields[1].hid_path, "UPS.PowerSummary.PresentStatus.Charging") == 0);
    assert(strcmp(fields[2].hid_path, "UPS.PowerSummary.PresentStatus.Discharging") == 0);
    assert(fields[3].report_id == 0x0C);
    assert(fields[3].type == HID_DESC_REPORT_FEATURE);
    assert(fields[3].bit_offset == 8);
    assert(fields[3].bit_size == 8);
    assert(strcmp(fields[3].nut_name, "battery.charge") == 0);
}

static void test_usage_range_expands_to_multiple_fields(void)
{
    const uint8_t descriptor[] = {
        0x05, 0x84, 0x09, 0x04, 0xA1, 0x01,
        0x09, 0x24, 0xA1, 0x02,
        0x85, 0x09,
        0x75, 0x01,
        0x95, 0x02,
        0x19, 0x57,       /* Usage Minimum Charging */
        0x29, 0x58,       /* Usage Maximum Discharging */
        0x81, 0x02,
        0xC0, 0xC0
    };
    hid_descriptor_field_t fields[4];
    size_t count = 0;
    assert(hid_descriptor_parse(descriptor, sizeof(descriptor), fields, 4, &count) == true);
    assert(count == 2);
    assert(strcmp(fields[0].hid_path, "UPS.PowerSummary.PresentStatus.Charging") == 0);
    assert(strcmp(fields[1].hid_path, "UPS.PowerSummary.PresentStatus.Discharging") == 0);
}

int main(void)
{
    test_simple_power_descriptor_paths();
    test_usage_range_expands_to_multiple_fields();
    return 0;
}
