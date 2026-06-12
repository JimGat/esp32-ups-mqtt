#ifndef HID_DESCRIPTOR_PARSER_H
#define HID_DESCRIPTOR_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HID_DESC_REPORT_INPUT = 1,
    HID_DESC_REPORT_OUTPUT = 2,
    HID_DESC_REPORT_FEATURE = 3,
} hid_desc_report_type_t;

typedef struct {
    hid_desc_report_type_t type;
    uint8_t report_id;
    uint16_t usage_page;
    uint16_t usage_id;
    uint16_t bit_offset;
    uint8_t bit_size;
    int32_t logical_min;
    int32_t logical_max;
    int8_t unit_exponent;
    char hid_path[96];
    char nut_name[40];
} hid_descriptor_field_t;

bool hid_descriptor_parse(const uint8_t *descriptor, size_t descriptor_len,
                          hid_descriptor_field_t *fields, size_t max_fields,
                          size_t *field_count);

const hid_descriptor_field_t *hid_descriptor_find_path(const hid_descriptor_field_t *fields,
                                                       size_t field_count,
                                                       const char *hid_path_suffix);
const char *hid_descriptor_report_type_str(hid_desc_report_type_t type);
const char *hid_descriptor_usage_name(uint16_t usage_page, uint16_t usage_id);

#ifdef __cplusplus
}
#endif

#endif /* HID_DESCRIPTOR_PARSER_H */
