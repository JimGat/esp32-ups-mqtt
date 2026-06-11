#ifndef UPS_PROFILE_H
#define UPS_PROFILE_H

#include <stdint.h>

// Runtime-selectable UPS protocol profile.
// Keep numeric values stable: they are stored in NVS as uint8_t.
typedef enum {
    UPS_PROFILE_AUTO = 0,
    UPS_PROFILE_APC_SMT2200 = 1,
    UPS_PROFILE_APC_GENERIC_HID = 2,
} ups_profile_t;

static inline const char *ups_profile_name(ups_profile_t profile)
{
    switch (profile) {
        case UPS_PROFILE_APC_SMT2200: return "APC Smart-UPS SMT2200";
        case UPS_PROFILE_APC_GENERIC_HID: return "Generic APC HID";
        case UPS_PROFILE_AUTO:
        default: return "Auto Detect";
    }
}

static inline const char *ups_profile_slug(ups_profile_t profile)
{
    switch (profile) {
        case UPS_PROFILE_APC_SMT2200: return "apc_smt2200";
        case UPS_PROFILE_APC_GENERIC_HID: return "apc_generic_hid";
        case UPS_PROFILE_AUTO:
        default: return "auto";
    }
}

static inline ups_profile_t ups_profile_validate(uint8_t raw)
{
    switch ((ups_profile_t)raw) {
        case UPS_PROFILE_AUTO:
        case UPS_PROFILE_APC_SMT2200:
        case UPS_PROFILE_APC_GENERIC_HID:
            return (ups_profile_t)raw;
        default:
            return UPS_PROFILE_AUTO;
    }
}

#endif // UPS_PROFILE_H
