#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip_addr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT BIT0
#define PROVISIONING_CHANNEL 6
#define PROVISIONING_MAX_CONN 4

static EventGroupHandle_t s_wifi_event_group;
static bool s_wifi_initialized = false;
static char stored_ssid[64] = {0};
static esp_netif_t *s_ap_netif = NULL;

static void sanitize_label_for_hostname(const char *input, char *output, size_t output_size)
{
    if (output_size == 0) {
        return;
    }
    output[0] = '\0';
    if (input == NULL || input[0] == '\0') {
        return;
    }

    size_t out_idx = 0;
    bool last_was_sep = false;
    for (size_t i = 0; input[i] != '\0' && out_idx < output_size - 1; i++) {
        char c = input[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            output[out_idx++] = c;
            last_was_sep = false;
        } else if ((c == ' ' || c == '-') && out_idx > 0 && !last_was_sep) {
            output[out_idx++] = '-';
            last_was_sep = true;
        }
    }
    if (out_idx > 0 && output[out_idx - 1] == '-') {
        out_idx--;
    }
    output[out_idx] = '\0';
}

static void event_handler(void* arg, esp_event_base_t event_base,
                         int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi disconnected, reconnecting...");
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
        ESP_LOGI(TAG, "Provisioning client connected: " MACSTR, MAC2STR(event->mac));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
        ESP_LOGI(TAG, "Provisioning client disconnected: " MACSTR, MAC2STR(event->mac));
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "✅ Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_common_init(void)
{
    if (s_wifi_initialized) {
        return ESP_OK;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        NULL));

    s_wifi_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_init_sta(const char *ssid, const char *password, const char *device_label)
{
    if (ssid == NULL || ssid[0] == '\0') {
        ESP_LOGE(TAG, "Cannot start station mode without an SSID");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_ERROR_CHECK(wifi_common_init());
    strlcpy(stored_ssid, ssid, sizeof(stored_ssid));
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    // Set hostname based on device_label if provided, otherwise use MAC-based hostname
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif != NULL) {
        char hostname[64];
        char sanitized_label[48];
        sanitize_label_for_hostname(device_label, sanitized_label, sizeof(sanitized_label));
        if (sanitized_label[0] != '\0') {
            snprintf(hostname, sizeof(hostname), "esp32-ups-%s", sanitized_label);
        } else {
            uint8_t mac[6];
            esp_efuse_mac_get_default(mac);
            snprintf(hostname, sizeof(hostname), "esp32-ups-mqtt-%02x%02x%02x", mac[3], mac[4], mac[5]);
        }
        esp_netif_set_hostname(sta_netif, hostname);
        ESP_LOGI(TAG, "Hostname set to: %s", hostname);
    }

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    if (password != NULL) {
        strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi station started, connecting to SSID: %s", ssid);
    return ESP_OK;
}

esp_err_t wifi_start_provisioning_ap(const char *ssid_prefix, const char *password)
{
    ESP_ERROR_CHECK(wifi_common_init());
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);

    char ap_ssid[33];
    const char *prefix = (ssid_prefix && ssid_prefix[0]) ? ssid_prefix : "ESP32-UPS-Setup";
    snprintf(ap_ssid, sizeof(ap_ssid), "%s-%02X%02X%02X", prefix, mac[3], mac[4], mac[5]);

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    ap_config.ap.channel = PROVISIONING_CHANNEL;
    ap_config.ap.max_connection = PROVISIONING_MAX_CONN;

    if (password != NULL && strlen(password) >= 8) {
        strlcpy((char *)ap_config.ap.password, password, sizeof(ap_config.ap.password));
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_err_t stop_err = esp_wifi_stop();
    if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_INIT && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_stop before AP returned: %s", esp_err_to_name(stop_err));
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t ip_info;
    if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        ESP_LOGW(TAG, "Provisioning AP started: SSID=%s URL=http://" IPSTR "/",
                 ap_ssid, IP2STR(&ip_info.ip));
    } else {
        ESP_LOGW(TAG, "Provisioning AP started: SSID=%s URL=http://192.168.4.1/", ap_ssid);
    }
    return ESP_OK;
}

esp_err_t wifi_wait_connected(uint32_t timeout_ms)
{
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "✅ Connected to WiFi SSID: %s", stored_ssid);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "❌ WiFi connection timeout");
    return ESP_ERR_TIMEOUT;
}

bool wifi_is_connected(void)
{
    if (s_wifi_event_group == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}
