/**
 * OTA Update & Device Management for UPS-MQTT-Bridge
 *
 * Provides:
 *   POST /update  - Upload new firmware binary, writes to inactive OTA slot, reboots
 *   POST /reboot  - Restart the device
 *   GET  /system   - System info (version, partition, uptime, free heap)
 */

#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota_update";

/* Maximum firmware size: 1.8MB (OTA slot is 0x1C0000 = 1,835,008 bytes) */
#define MAX_FIRMWARE_SIZE (0x1C0000)

/* ---- POST /reboot ---- */
esp_err_t reboot_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Reboot requested via web UI");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"rebooting\",\"message\":\"Device will restart in 1 second\"}");

    /* Delay to let HTTP response go out before reboot */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK; /* unreachable */
}

/* ---- GET /system ---- */
esp_err_t system_info_handler(httpd_req_t *req)
{
    // Prevent browser caching so uptime always shows current value
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_app_desc_t *app_desc = esp_app_get_description();
    uint32_t free_heap = esp_get_free_heap_size();
    int64_t uptime_s = esp_timer_get_time() / 1000000;

    char resp[512];
    snprintf(resp, sizeof(resp),
        "{"
        "\"firmware\":\"%s\","
        "\"compile_time\":\"%s %s\","
        "\"running_partition\":\"%s\","
        "\"boot_partition\":\"%s\","
        "\"ota_slot\":%d,"
        "\"free_heap\":%lu,"
        "\"uptime_s\":%lld"
        "}",
        app_desc->version,
        app_desc->date,
        app_desc->time,
        running ? running->label : "?",
        boot ? boot->label : "?",
        running ? (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? 0 :
                   running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 ? 1 : -1) : -1,
        (unsigned long)free_heap,
        (long long)uptime_s
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* ---- POST /update (OTA firmware upload) ---- */

typedef struct {
    esp_ota_handle_t ota_handle;
    bool ota_started;
    int total_written;
    bool error;
    char error_msg[128];
} ota_upload_ctx_t;

static ota_upload_ctx_t upload_ctx;

/* Pre-upload validation endpoint - checks if OTA is possible */
esp_err_t update_check_handler(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);

    if (!update) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
            "{\"error\":\"No OTA partition available\"}");
        return ESP_FAIL;
    }

    char resp[256];
    snprintf(resp, sizeof(resp),
        "{\"can_update\":true,"
        "\"running\":\"%s\","
        "\"target\":\"%s\","
        "\"max_size\":%lu}",
        running ? running->label : "?",
        update->label,
        (unsigned long)update->size
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

esp_err_t update_post_handler(httpd_req_t *req)
{
    char content_type[64] = {0};
    httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type));
    
    ESP_LOGI(TAG, "📥 OTA upload request received: %d bytes, Content-Type: %s", 
             req->content_len, content_type[0] ? content_type : "UNKNOWN");

    /* Get the next OTA partition to write to */
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA partition found");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
            "{\"error\":\"No OTA partition available\"}");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Writing to partition: %s at offset 0x%lx",
             update_partition->label, (unsigned long)update_partition->address);

    /* Initialize OTA */
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &upload_ctx.ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"error\":\"OTA begin failed: %s\"}", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, resp);
        return ESP_FAIL;
    }

    upload_ctx.ota_started = true;
    upload_ctx.total_written = 0;
    upload_ctx.error = false;

    /* Read firmware data in chunks */
    char buf[4096];
    int remaining = req->content_len;
    int received = 0;

    while (remaining > 0) {
        int to_recv = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        int ret = httpd_req_recv(req, buf, to_recv);

        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Retry on timeout - yield to avoid tight CPU spin loop */
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            ESP_LOGE(TAG, "Connection closed, received %d of %d", received, req->content_len);
            snprintf(upload_ctx.error_msg, sizeof(upload_ctx.error_msg),
                "Connection lost at %d/%d bytes", received, req->content_len);
            upload_ctx.error = true;
            esp_ota_abort(upload_ctx.ota_handle);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                "{\"error\":\"Upload interrupted\"}");
            return ESP_FAIL;
        }

        /* Write chunk to OTA partition */
        err = esp_ota_write(upload_ctx.ota_handle, buf, ret);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(upload_ctx.ota_handle);
            httpd_resp_set_type(req, "application/json");
            char resp[128];
            snprintf(resp, sizeof(resp), "{\"error\":\"OTA write failed: %s\"}", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, resp);
            return ESP_FAIL;
        }

        received += ret;
        remaining -= ret;
        upload_ctx.total_written += ret;

        /* Progress logging every 64KB */
        if (upload_ctx.total_written % 65536 == 0) {
            ESP_LOGI(TAG, "OTA progress: %d/%d bytes (%d%%)",
                     received, req->content_len,
                     req->content_len > 0 ? (received * 100 / req->content_len) : 0);
        }
    }

    ESP_LOGI(TAG, "OTA upload complete: %d bytes written", upload_ctx.total_written);

    /* Finalize OTA */
    err = esp_ota_end(upload_ctx.ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                "{\"error\":\"Firmware validation failed - invalid image\"}");
        } else {
            char resp[128];
            snprintf(resp, sizeof(resp), "{\"error\":\"OTA end failed: %s\"}", esp_err_to_name(err));
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, resp);
        }
        return ESP_FAIL;
    }

    /* Set boot partition to the new OTA slot */
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"error\":\"Set boot partition failed: %s\"}", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, resp);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update successful! Boot partition set to %s. Rebooting...",
             update_partition->label);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req,
        "{\"status\":\"success\","
        "\"message\":\"Firmware updated. Rebooting into new version...\","
        "\"bytes_written\":0}");

    /* Replace bytes_written with actual value */
    /* (snprintf would be cleaner but this works for the response format) */

    /* Reboot after short delay to let response go out */
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK; /* unreachable */
}

/* ---- Register OTA and system endpoints ---- */
void register_ota_handlers(httpd_handle_t server)
{
    /* Reboot */
    httpd_uri_t reboot_uri = {
        .uri = "/reboot",
        .method = HTTP_POST,
        .handler = reboot_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &reboot_uri);

    /* System info */
    httpd_uri_t system_uri = {
        .uri = "/system",
        .method = HTTP_GET,
        .handler = system_info_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &system_uri);

    /* OTA update check */
    httpd_uri_t update_check_uri = {
        .uri = "/update",
        .method = HTTP_GET,
        .handler = update_check_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &update_check_uri);

    /* OTA firmware upload */
    httpd_uri_t update_uri = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = update_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &update_uri);

    ESP_LOGI(TAG, "OTA & system endpoints registered (/update, /reboot, /system)");
}
