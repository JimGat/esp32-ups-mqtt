#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../main/power_event.h"

static ups_metrics_t metrics(bool online, bool discharging, bool low_battery)
{
    ups_metrics_t m;
    memset(&m, 0, sizeof(m));
    m.valid = true;
    m.status.online = online;
    m.status.discharging = discharging;
    m.status.low_battery = low_battery;
    m.input_voltage = online ? 121.0f : 0.0f;
    m.battery_charge = low_battery ? 9.0f : 98.0f;
    m.battery_runtime = low_battery ? 95.0f : 2400.0f;
    m.load_percent = 23.0f;
    snprintf(m.status_string, sizeof(m.status_string), "%s", discharging ? "On Battery" : "Online");
    return m;
}

static void test_power_lost_when_device_starts_discharging(void)
{
    power_event_state_t state;
    power_event_state_init(&state);

    ups_metrics_t online = metrics(true, false, false);
    assert(power_event_classify(&state, &online) == POWER_EVENT_NONE);

    ups_metrics_t on_battery = metrics(false, true, false);
    assert(power_event_classify(&state, &on_battery) == POWER_EVENT_POWER_LOST);
    assert(power_event_classify(&state, &on_battery) == POWER_EVENT_NONE);
}

static void test_power_restored_when_line_returns(void)
{
    power_event_state_t state;
    power_event_state_init(&state);

    ups_metrics_t on_battery = metrics(false, true, false);
    ups_metrics_t online = metrics(true, false, false);

    assert(power_event_classify(&state, &on_battery) == POWER_EVENT_NONE);
    assert(power_event_classify(&state, &online) == POWER_EVENT_POWER_RESTORED);
    assert(power_event_classify(&state, &online) == POWER_EVENT_NONE);
}

static void test_low_battery_assert_and_clear_events(void)
{
    power_event_state_t state;
    power_event_state_init(&state);

    ups_metrics_t normal = metrics(false, true, false);
    ups_metrics_t low = metrics(false, true, true);

    assert(power_event_classify(&state, &normal) == POWER_EVENT_NONE);
    assert(power_event_classify(&state, &low) == POWER_EVENT_LOW_BATTERY);
    assert(power_event_classify(&state, &low) == POWER_EVENT_NONE);
    assert(power_event_classify(&state, &normal) == POWER_EVENT_LOW_BATTERY_CLEARED);
}

static void test_event_names_are_stable_for_mqtt_payloads(void)
{
    assert(strcmp(power_event_name(POWER_EVENT_POWER_LOST), "power_lost") == 0);
    assert(strcmp(power_event_name(POWER_EVENT_POWER_RESTORED), "power_restored") == 0);
    assert(strcmp(power_event_name(POWER_EVENT_LOW_BATTERY), "low_battery") == 0);
    assert(strcmp(power_event_name(POWER_EVENT_LOW_BATTERY_CLEARED), "low_battery_cleared") == 0);
    assert(strcmp(power_event_name(POWER_EVENT_NONE), "none") == 0);
}

int main(void)
{
    test_power_lost_when_device_starts_discharging();
    test_power_restored_when_line_returns();
    test_low_battery_assert_and_clear_events();
    test_event_names_are_stable_for_mqtt_payloads();
    puts("power_event tests passed");
    return 0;
}
