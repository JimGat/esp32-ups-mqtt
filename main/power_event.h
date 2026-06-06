#ifndef POWER_EVENT_H
#define POWER_EVENT_H

#include <stdbool.h>
#include "apc_hid_parser.h"

typedef enum {
    POWER_EVENT_NONE = 0,
    POWER_EVENT_POWER_LOST,
    POWER_EVENT_POWER_RESTORED,
    POWER_EVENT_LOW_BATTERY,
    POWER_EVENT_LOW_BATTERY_CLEARED,
} power_event_type_t;

typedef struct {
    bool initialized;
    bool was_on_line;
    bool was_low_battery;
} power_event_state_t;

void power_event_state_init(power_event_state_t *state);
power_event_type_t power_event_classify(power_event_state_t *state, const ups_metrics_t *metrics);
const char *power_event_name(power_event_type_t event);

#endif // POWER_EVENT_H
