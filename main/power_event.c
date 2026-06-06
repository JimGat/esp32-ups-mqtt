#include "power_event.h"

static bool metrics_on_line(const ups_metrics_t *metrics)
{
    return metrics->status.online && !metrics->status.discharging;
}

void power_event_state_init(power_event_state_t *state)
{
    state->initialized = false;
    state->was_on_line = false;
    state->was_low_battery = false;
}

power_event_type_t power_event_classify(power_event_state_t *state, const ups_metrics_t *metrics)
{
    if (state == NULL || metrics == NULL || !metrics->valid) {
        return POWER_EVENT_NONE;
    }

    const bool on_line = metrics_on_line(metrics);
    const bool low_battery = metrics->status.low_battery;

    if (!state->initialized) {
        state->initialized = true;
        state->was_on_line = on_line;
        state->was_low_battery = low_battery;
        return POWER_EVENT_NONE;
    }

    if (state->was_on_line && !on_line) {
        state->was_on_line = on_line;
        return POWER_EVENT_POWER_LOST;
    }

    if (!state->was_on_line && on_line) {
        state->was_on_line = on_line;
        return POWER_EVENT_POWER_RESTORED;
    }

    if (!state->was_low_battery && low_battery) {
        state->was_low_battery = low_battery;
        return POWER_EVENT_LOW_BATTERY;
    }

    if (state->was_low_battery && !low_battery) {
        state->was_low_battery = low_battery;
        return POWER_EVENT_LOW_BATTERY_CLEARED;
    }

    state->was_on_line = on_line;
    state->was_low_battery = low_battery;
    return POWER_EVENT_NONE;
}

const char *power_event_name(power_event_type_t event)
{
    switch (event) {
    case POWER_EVENT_POWER_LOST:
        return "power_lost";
    case POWER_EVENT_POWER_RESTORED:
        return "power_restored";
    case POWER_EVENT_LOW_BATTERY:
        return "low_battery";
    case POWER_EVENT_LOW_BATTERY_CLEARED:
        return "low_battery_cleared";
    case POWER_EVENT_NONE:
    default:
        return "none";
    }
}
