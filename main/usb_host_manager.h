#ifndef USB_HOST_MANAGER_H
#define USB_HOST_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "ups_profile.h"
#include "nut_runtime_map.h"

void usb_host_set_configured_profile(ups_profile_t profile);
ups_profile_t usb_host_get_active_profile(void);
esp_err_t usb_host_init(void);
esp_err_t usb_host_install_only_diag(void);
esp_err_t usb_host_register_client_only_diag(void);
void usb_host_task(void *arg);
void usb_host_lib_events_only_task(void *arg);
bool usb_ups_is_connected(void);

typedef enum {
    USB_DEBUG_MODE_OFF = 0,
    USB_DEBUG_MODE_PASSIVE = 1,
    USB_DEBUG_MODE_ACTIVE = 2,
} usb_debug_mode_t;

typedef enum {
    USB_DEBUG_REC_EVENT = 0,
    USB_DEBUG_REC_INTERRUPT = 1,
    USB_DEBUG_REC_FEATURE = 2,
    USB_DEBUG_REC_DESCRIPTOR = 3,
    USB_DEBUG_REC_ERROR = 4,
} usb_debug_record_type_t;

#define USB_DEBUG_MAX_RECORD_DATA 64

typedef struct {
    usb_debug_mode_t mode;
    bool capture_interrupt_reports;
    bool capture_feature_reports;
    bool include_control_setup;
    bool log_to_esp_log;
} usb_debug_config_t;

typedef struct {
    bool ups_connected;
    usb_debug_mode_t mode;
    uint32_t interrupt_reports_seen;
    uint32_t feature_reports_seen;
    uint32_t descriptor_dumps;
    uint32_t errors;
    uint32_t dropped_records;
    int64_t last_activity_ms;
} usb_debug_state_t;

typedef struct {
    uint32_t seq;
    int64_t timestamp_ms;
    usb_debug_record_type_t type;
    uint8_t report_id;
    uint8_t status;
    uint16_t length;
    uint8_t data[USB_DEBUG_MAX_RECORD_DATA];
    char note[80];
} usb_debug_record_t;

esp_err_t usb_debug_set_config(const usb_debug_config_t *cfg);
void usb_debug_get_config(usb_debug_config_t *cfg);
void usb_debug_get_state(usb_debug_state_t *state);
size_t usb_debug_get_records(usb_debug_record_t *out, size_t max_records, uint32_t since_seq);
void usb_debug_clear_records(void);
esp_err_t usb_debug_request_descriptor(void);
uint16_t usb_debug_safe_report_length(uint8_t report_id, uint16_t requested_length);
esp_err_t usb_debug_request_report(uint8_t report_type, uint8_t report_id, uint16_t length);
esp_err_t usb_debug_request_report_safe(uint8_t report_type, uint8_t report_id, uint16_t requested_length);

size_t usb_debug_get_runtime_map(nut_runtime_map_entry_t *out, size_t max_entries,
                                 uint32_t *version, uint16_t *descriptor_len);

#endif // USB_HOST_MANAGER_H
