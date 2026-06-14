#include "http_server.h"
#include "apc_hid_parser.h"
#include "usb_host_manager.h"
#include "wifi_manager.h"
#include "ota_update.h"
#include "ups_hid_map.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "http_server";

/* ═══════════════ Log Ring Buffer ═══════════════ */
/* Bridge buffer -- holds logs between JS polls (every 2s).
   The browser DOM is the real long-term buffer (up to 2000 lines).
   200 lines covers ~2 min of verbose logging, plenty for 2s polls. */
#define LOG_RING_SIZE 200
#define LOG_LINE_LEN  100

static char log_ring[LOG_RING_SIZE][LOG_LINE_LEN];
static int  log_write_idx = 0;
static int  log_count     = 0;
static SemaphoreHandle_t log_mutex = NULL;
static vprintf_like_t    original_vprintf_fn = NULL;

static app_config_t *current_config = NULL;

#define WEB_AUTH_USER "admin"
#define DEFAULT_PROVISIONING_PASSWORD CONFIG_PROVISIONING_AP_PASSWORD
#define PAGE_TITLE "UPS MQTT Bridge"
#define STATUS_TITLE "UPS MQTT Bridge Status"
#ifndef FW_VERSION
#define FW_VERSION "v0.0.0-unknown"
#endif

static int capture_vprintf(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);

    if (log_mutex && xSemaphoreTake(log_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        vsnprintf(log_ring[log_write_idx], LOG_LINE_LEN, fmt, copy);
        int len = strlen(log_ring[log_write_idx]);
        if (len > 0 && log_ring[log_write_idx][len - 1] == '\n')
            log_ring[log_write_idx][len - 1] = '\0';
        log_write_idx = (log_write_idx + 1) % LOG_RING_SIZE;
        if (log_count < LOG_RING_SIZE) log_count++;
        xSemaphoreGive(log_mutex);
    }
    va_end(copy);

    if (original_vprintf_fn)
        return original_vprintf_fn(fmt, args);
    return vprintf(fmt, args);
}

/* ═══════════════ URL Decode / Form Parse ═══════════════ */

static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t di = 0;
    while (*src && di < dst_size - 1) {
        if (*src == '+') {
            dst[di++] = ' '; src++;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[di++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            dst[di++] = *src++;
        }
    }
    dst[di] = '\0';
}

static bool get_form_value(const char *body, const char *key, char *value, size_t value_size)
{
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);

    const char *start = strstr(body, search);
    if (!start) return false;

    /* make sure we matched the full key, not a suffix of another key */
    if (start != body && *(start - 1) != '&') {
        /* search again after first hit */
        start = strstr(start + 1, search);
        if (!start) return false;
    }

    start += strlen(search);
    const char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);

    char encoded[256];
    if (len >= sizeof(encoded)) len = sizeof(encoded) - 1;
    memcpy(encoded, start, len);
    encoded[len] = '\0';
    url_decode(value, encoded, value_size);
    return true;
}

/* ═══════════════ Config Load / Save (NVS) ═══════════════ */

esp_err_t config_load(app_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->publish_interval_ms = CONFIG_MQTT_PUBLISH_INTERVAL_MS;
    config->ups_profile = UPS_PROFILE_AUTO;
    strlcpy(config->provisioning_pass, DEFAULT_PROVISIONING_PASSWORD, sizeof(config->provisioning_pass));
    strlcpy(config->web_pass, DEFAULT_PROVISIONING_PASSWORD, sizeof(config->web_pass));

    nvs_handle_t nvs;
    if (nvs_open("config", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len;
        len = sizeof(config->wifi_ssid);  nvs_get_str(nvs, "wifi_ssid", config->wifi_ssid, &len);
        len = sizeof(config->wifi_pass);  nvs_get_str(nvs, "wifi_pass", config->wifi_pass, &len);
        len = sizeof(config->mqtt_url);   nvs_get_str(nvs, "mqtt_url",  config->mqtt_url,  &len);
        len = sizeof(config->mqtt_user);  nvs_get_str(nvs, "mqtt_user", config->mqtt_user, &len);
        len = sizeof(config->mqtt_pass);  nvs_get_str(nvs, "mqtt_pass", config->mqtt_pass, &len);
        len = sizeof(config->provisioning_pass); nvs_get_str(nvs, "prov_pass", config->provisioning_pass, &len);
        len = sizeof(config->web_pass);   nvs_get_str(nvs, "web_pass",  config->web_pass,  &len);
        len = sizeof(config->device_label); nvs_get_str(nvs, "device_label", config->device_label, &len);
        nvs_get_u32(nvs, "pub_interval", &config->publish_interval_ms);
        nvs_get_u8(nvs, "ups_profile", &config->ups_profile);
        config->ups_profile = (uint8_t)ups_profile_validate(config->ups_profile);
        nvs_close(nvs);
        if (config->provisioning_pass[0] == '\0') {
            const char *fallback_pass = config->web_pass[0] ? config->web_pass : DEFAULT_PROVISIONING_PASSWORD;
            strlcpy(config->provisioning_pass, fallback_pass, sizeof(config->provisioning_pass));
        }
        if (config->web_pass[0] == '\0') {
            strlcpy(config->web_pass, config->provisioning_pass, sizeof(config->web_pass));
        }
        ESP_LOGI(TAG, "Config loaded from NVS");
    } else {
        ESP_LOGW(TAG, "No saved NVS config found; provisioning is required");
    }

    return ESP_OK;
}

bool config_is_complete(const app_config_t *config)
{
    return config != NULL &&
           config->wifi_ssid[0] != '\0' &&
           config->mqtt_url[0] != '\0';
}

static esp_err_t config_save(const app_config_t *config)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("config", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    nvs_set_str(nvs, "wifi_ssid", config->wifi_ssid);
    nvs_set_str(nvs, "wifi_pass", config->wifi_pass);
    nvs_set_str(nvs, "mqtt_url",  config->mqtt_url);
    nvs_set_str(nvs, "mqtt_user", config->mqtt_user);
    nvs_set_str(nvs, "mqtt_pass", config->mqtt_pass);
    nvs_set_str(nvs, "prov_pass", config->provisioning_pass);
    nvs_set_str(nvs, "web_pass",  config->web_pass);
    nvs_set_str(nvs, "device_label", config->device_label);
    nvs_set_u32(nvs, "pub_interval", config->publish_interval_ms);
    nvs_set_u8(nvs, "ups_profile", ups_profile_validate(config->ups_profile));

    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

/* ═══════════════ Basic Auth Helpers ═══════════════ */

static void send_auth_challenge(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"UPS MQTT Bridge\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Authentication required. Username: admin");
}

static bool check_basic_auth(httpd_req_t *req)
{
    if (current_config == NULL) {
        send_auth_challenge(req);
        return false;
    }

    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hdr_len == 0 || hdr_len >= 192) {
        send_auth_challenge(req);
        return false;
    }

    char auth_header[192];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) != ESP_OK) {
        send_auth_challenge(req);
        return false;
    }

    const char prefix[] = "Basic ";
    if (strncmp(auth_header, prefix, strlen(prefix)) != 0) {
        send_auth_challenge(req);
        return false;
    }

    unsigned char decoded[128];
    size_t decoded_len = 0;
    int rc = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                                   (const unsigned char *)(auth_header + strlen(prefix)),
                                   strlen(auth_header + strlen(prefix)));
    if (rc != 0 || decoded_len >= sizeof(decoded)) {
        send_auth_challenge(req);
        return false;
    }
    decoded[decoded_len] = '\0';

    const char *password = current_config->web_pass[0] ? current_config->web_pass : current_config->provisioning_pass;
    char expected[96];
    snprintf(expected, sizeof(expected), "%s:%s", WEB_AUTH_USER, password);

    if (strcmp((const char *)decoded, expected) != 0) {
        ESP_LOGW(TAG, "HTTP auth failed");
        send_auth_challenge(req);
        return false;
    }

    return true;
}

/* ═══════════════ HTML Helpers ═══════════════ */

static void html_escape(char *dst, const char *src, size_t dst_size)
{
    size_t di = 0;
    while (*src && di < dst_size - 6) {
        switch (*src) {
            case '<':  di += snprintf(dst + di, dst_size - di, "&lt;");   break;
            case '>':  di += snprintf(dst + di, dst_size - di, "&gt;");   break;
            case '&':  di += snprintf(dst + di, dst_size - di, "&amp;");  break;
            case '"':  di += snprintf(dst + di, dst_size - di, "&quot;"); break;
            default:   dst[di++] = *src;
        }
        src++;
    }
    dst[di] = '\0';
}

static const char *PAGE_STYLE =
    "body{font-family:sans-serif;max-width:700px;margin:0 auto;padding:20px;background:#1a1a2e;color:#e0e0e0}"
    "h1{color:#4db8ff;text-align:center}h2{color:#a0a0c0;border-bottom:1px solid #333;padding-bottom:5px}"
    "input,select{width:100%;padding:8px;margin:5px 0 15px;box-sizing:border-box;"
        "background:#16213e;color:#e0e0e0;border:1px solid #333;border-radius:4px}"
    "label{font-weight:bold;color:#a0a0c0}"
    "button{background:#0f3460;color:white;padding:12px 24px;border:none;border-radius:4px;"
        "cursor:pointer;width:100%;font-size:16px}"
    "button:hover{background:#1a508b}"
    ".card{background:#16213e;padding:15px;border-radius:8px;margin:15px 0}"
    "table{width:100%;border-collapse:collapse}"
    "td,th{padding:6px 10px;text-align:left;border-bottom:1px solid #333}"
    "th{color:#a0a0c0;width:40%}.val{color:#4db8ff;font-weight:bold}"
    "pre{background:#0d1117;color:#c9d1d9;padding:12px;border-radius:6px;"
        "overflow-x:auto;font-size:12px;max-height:500px;overflow-y:auto;line-height:1.4}"
    "a{color:#4db8ff;text-decoration:none}a:hover{text-decoration:underline}"
    ".nav{text-align:center;margin:10px 0}"
    ".version-line{text-align:center;margin:-4px 0 12px;color:#a0a0c0;font-size:14px}"
    ".version-line strong{color:#4db8ff}"
    ".online{color:#4caf50}.offline{color:#f44336}";

static const char *PAGE_NAV =
    "<div class='nav'><a href='/'>Config</a> | <a href='/status'>Status &amp; Logs</a> | <a href='/usb-debug'>USB Debug</a></div>";

/* ═══════════════ GET / — Config Page ═══════════════ */

static void send_page_header(httpd_req_t *req, const char *title, bool auto_refresh)
{
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>");
    if (auto_refresh) {
        httpd_resp_sendstr_chunk(req, "<meta http-equiv='refresh' content='5'>");
    }
    httpd_resp_sendstr_chunk(req, "<title>");
    httpd_resp_sendstr_chunk(req, title);
    httpd_resp_sendstr_chunk(req, "</title><style>");
    httpd_resp_sendstr_chunk(req, PAGE_STYLE);
    httpd_resp_sendstr_chunk(req, "</style></head><body><h1>");
    httpd_resp_sendstr_chunk(req, title);
    httpd_resp_sendstr_chunk(req, "</h1><div class=version-line>Firmware <strong>");
    httpd_resp_sendstr_chunk(req, FW_VERSION);
    httpd_resp_sendstr_chunk(req, "</strong> &middot; <a href=/version>Version JSON</a></div>");
    httpd_resp_sendstr_chunk(req, PAGE_NAV);
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    char buf[1024];

    if (!check_basic_auth(req)) return ESP_OK;

    send_page_header(req, PAGE_TITLE, false);

    char ssid_esc[128], url_esc[256], user_esc[128];
    html_escape(ssid_esc, current_config->wifi_ssid, sizeof(ssid_esc));
    html_escape(url_esc,  current_config->mqtt_url,  sizeof(url_esc));
    html_escape(user_esc, current_config->mqtt_user,  sizeof(user_esc));

    httpd_resp_sendstr_chunk(req, "<form method='POST' action='/save'>");

    /* WiFi */
    snprintf(buf, 1024,
        "<div class='card'><h2>WiFi</h2>"
        "<label>SSID</label><input name='wifi_ssid' value='%s'>"
        "<label>Password</label><input name='wifi_pass' type='password' value='%s'>"
        "</div>",
        ssid_esc, current_config->wifi_pass);
    httpd_resp_sendstr_chunk(req, buf);

    /* MQTT */
    httpd_resp_sendstr_chunk(req, "<div class='card'><h2>MQTT</h2>");
    snprintf(buf, 1024,
        "<label>Broker URL</label><input name='mqtt_url' value='%s'>", url_esc);
    httpd_resp_sendstr_chunk(req, buf);
    snprintf(buf, 1024,
        "<label>Username</label><input name='mqtt_user' value='%s'>", user_esc);
    httpd_resp_sendstr_chunk(req, buf);
    snprintf(buf, 1024,
        "<label>Password</label><input name='mqtt_pass' type='password' value='%s'>",
        current_config->mqtt_pass);
    httpd_resp_sendstr_chunk(req, buf);
    httpd_resp_sendstr_chunk(req, "</div>");

    /* Web Interface */
    httpd_resp_sendstr_chunk(req,
        "<div class='card'><h2>Config AP / Admin Access</h2>"
        "<p>Username is <strong>admin</strong>. This password also protects the fallback setup AP. Leave blank to keep the current password.</p>"
        "<label>New Config AP/Admin Password</label><input name='admin_pass' type='password' placeholder='Leave blank to keep current password'>"
        "</div>");

    /* Device Label */
    char label_esc[128];
    html_escape(label_esc, current_config->device_label, sizeof(label_esc));
    snprintf(buf, 1024,
        "<div class='card'><h2>Device Identification</h2>"
        "<label>Device Label (e.g., 'UPS Server Room')</label><input name='device_label' value='%s'>"
        "<label>UPS Make / Model Profile</label>"
        "<select name='ups_profile'>"
        "<option value='0' %s>Auto Detect (best effort)</option>"
        "<option value='1' %s>APC Smart-UPS SMT2200</option>"
        "<option value='2' %s>Generic APC HID</option>"
        "</select>"
        "<p class='muted'>Profiles control report polling and decoding. Auto selects SMT2200 for APC VID:PID 051D:0003; otherwise generic APC HID.</p>"
        "</div>",
        label_esc,
        current_config->ups_profile == UPS_PROFILE_AUTO ? "selected" : "",
        current_config->ups_profile == UPS_PROFILE_APC_SMT2200 ? "selected" : "",
        current_config->ups_profile == UPS_PROFILE_APC_GENERIC_HID ? "selected" : "");
    httpd_resp_sendstr_chunk(req, buf);

    /* Interval */
    snprintf(buf, 1024,
        "<div class='card'><h2>Publish Interval</h2>"
        "<label>Seconds</label>"
        "<input name='interval' type='number' min='5' max='300' value='%lu'>"
        "</div>",
        (unsigned long)(current_config->publish_interval_ms / 1000));
    httpd_resp_sendstr_chunk(req, buf);

    httpd_resp_sendstr_chunk(req,
        "<button type='submit'>Save &amp; Reboot</button></form></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ═══════════════ GET /status — Metrics + Logs ═══════════════ */

static esp_err_t status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    char buf[512];

    if (!check_basic_auth(req)) return ESP_OK;

    send_page_header(req, STATUS_TITLE, false);

    /* UPS Metrics */
    ups_metrics_t metrics_snapshot = apc_hid_get_metrics_snapshot();
    const ups_metrics_t *m = &metrics_snapshot;
    ups_transport_stats_t stats = ups_transport_stats_get();
    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t last_poll_age_ms = (stats.last_poll_ms > 0 && now_ms >= stats.last_poll_ms) ? (now_ms - stats.last_poll_ms) : 0;
    const char *transport_health = (stats.get_report_error || stats.get_report_timeout) ? "DEGRADED" : "OK";

    httpd_resp_sendstr_chunk(req,
        "<div class='card'><h2>UPS Metrics</h2><table id='mtbl'>");

    if (m->valid) {
        snprintf(buf, 1024,
            "<tr><th>Status</th><td class='val %s'>%s</td></tr>"
            "<tr><th>Status Confidence</th><td class='val'>%s</td></tr>",
            m->status.online ? "online" : "offline",
            m->status_string,
            m->status_confidence);
        httpd_resp_sendstr_chunk(req, buf);

        snprintf(buf, 1024,
            "<tr><th>Active Profile</th><td class='val'>%s / NUT-style HID</td></tr>"
            "<tr><th>Transport Health</th><td class='val'>%s</td></tr>"
            "<tr><th>Last Poll Age</th><td class='val'>%lu ms</td></tr>",
            ups_profile_name(apc_hid_parser_get_profile()),
            transport_health,
            (unsigned long)last_poll_age_ms);
        httpd_resp_sendstr_chunk(req, buf);

        snprintf(buf, 1024,
            "<tr><th>Battery Charge</th><td class='val'>%.0f%%</td></tr>"
            "<tr><th>Battery Voltage</th><td class='val'>%.1f V</td></tr>"
            "<tr><th>Battery Runtime</th><td class='val'>%.0f s (%.1f min)</td></tr>",
            m->battery_charge,
            m->battery_voltage,
            m->battery_runtime, m->battery_runtime / 60.0f);
        httpd_resp_sendstr_chunk(req, buf);

        snprintf(buf, 1024,
            "<tr><th>Input Voltage</th><td class='val'>%.0f V</td></tr>"
            "<tr><th>Load</th><td class='val'>%.0f%%</td></tr>",
            m->input_voltage, m->load_percent);
        httpd_resp_sendstr_chunk(req, buf);

        if (strcmp(m->status_confidence, "confirmed") != 0) {
            httpd_resp_sendstr_chunk(req, "<tr><td colspan='2' class='warn'>Status not descriptor-confirmed. Canonical UPS status remains UNKNOWN until NUT/descriptor PresentStatus or APCLineFailCause mapping is validated.</td></tr>");
        }
        if (m->nominal_power > 0) {
            snprintf(buf, 1024,
                "<tr><th>Nominal Power</th><td class='val'>%.0f W</td></tr>",
                m->nominal_power);
            httpd_resp_sendstr_chunk(req, buf);
        }
        if (m->input_voltage_nominal > 0) {
            snprintf(buf, 1024,
                "<tr><th>Nominal Input</th><td class='val'>%.0f V</td></tr>",
                m->input_voltage_nominal);
            httpd_resp_sendstr_chunk(req, buf);
        }
        if (strlen(m->beeper_status) > 0) {
            snprintf(buf, 1024,
                "<tr><th>Beeper</th><td class='val'>%s</td></tr>",
                m->beeper_status);
            httpd_resp_sendstr_chunk(req, buf);
        }
    } else {
        httpd_resp_sendstr_chunk(req,
            "<tr><td colspan='2'>No valid UPS data available</td></tr>");
    }

    httpd_resp_sendstr_chunk(req, "</table></div>");

    /* Connection Info */
    snprintf(buf, 1024,
        "<div class='card'><h2>Connection</h2><table>"
        "<tr><th>WiFi</th><td class='val'>%s</td></tr>"
        "<tr><th>MQTT Broker</th><td class='val'>%s</td></tr>"
        "<tr><th>USB UPS</th><td class='val %s'>%s</td></tr>"
        "<tr><th>Publish Interval</th><td class='val'>%lu s</td></tr>"
        "</table></div>",
        current_config->wifi_ssid,
        current_config->mqtt_url,
        usb_ups_is_connected() ? "online" : "offline",
        usb_ups_is_connected() ? "Connected" : "Disconnected",
        (unsigned long)(current_config->publish_interval_ms / 1000));
    httpd_resp_sendstr_chunk(req, buf);

    /* Serial Logs (JS-powered: polls /logs endpoint, auto-scrolls, copy button) */
    httpd_resp_sendstr_chunk(req,
        "<div class='card'><h2>Serial Logs</h2>"
        "<button id='cpb' onclick=\"copyLogs()\" style='margin-bottom:8px;padding:4px 12px;cursor:pointer;border-radius:4px;border:1px solid #555;background:#333;color:#eee'>Copy Logs</button>"
        "<button id='cls' onclick=\"document.getElementById('logs').textContent='';logIdx=0\" style='margin-left:4px;padding:4px 12px;cursor:pointer;border-radius:4px;border:1px solid #555;background:#333;color:#eee'>Clear</button>"
        "<pre id='logs' style='max-height:500px;overflow-y:auto;font-size:12px'></pre>"
        "</div>"
        /* Device Management Card (HTML) */
        "<div class='card'><h2>Device Management</h2>"
        "<table id='systbl'><tr><th>Firmware</th><td>loading...</td></tr></table>"
        "<hr style='border-color:#444'>"
        "<button id='rbt' onclick='doReboot()' style='padding:4px 12px;cursor:pointer;border-radius:4px;border:1px solid #855;background:#543;color:#fcc;margin:4px'>Reboot Device</button>"
        "<hr style='border-color:#444'>"
        "<h3 style='margin:4px 0'>Firmware Update (OTA)</h3>"
        "<input type='file' id='fwfile' accept='.bin' style='margin:4px 0'>"
        "<button id='updbtn' onclick='doUpdate()' style='padding:4px 12px;cursor:pointer;border-radius:4px;border:1px solid #585;background:#353;color:#cfc;margin:4px'>Upload Firmware</button>"
        "</div>"
        "<script>"
        "var logIdx=0,MAX_DOM=2000;"
        "function copyLogs(){"
        "  var p=document.getElementById('logs');"
        "  var t=p.innerText||p.textContent||'';"
        "  if(!t||t.trim()===''){var b=document.getElementById('cpb');b.textContent='No logs!';setTimeout(function(){b.textContent='Copy Logs'},2000);return;}"
        "  if(navigator.clipboard&&window.isSecureContext){"
        "    navigator.clipboard.writeText(t).then(function(){var b=document.getElementById('cpb');b.textContent='Copied!';setTimeout(function(){b.textContent='Copy Logs'},2000);}).catch(function(){fallbackCopy(p);});"
        "  }else{"
        "    fallbackCopy(p);"
        "  }"
        "}"
        "function fallbackCopy(node){"
        "  var range=document.createRange();"
        "  range.selectNodeContents(node);"
        "  var sel=window.getSelection();"
        "  sel.removeAllRanges();"
        "  sel.addRange(range);"
        "  try{"
        "    var ok=document.execCommand('copy');"
        "    var b=document.getElementById('cpb');"
        "    b.textContent=ok?'Copied!':'Copy failed';"
        "    setTimeout(function(){b.textContent='Copy Logs'},2000);"
        "  }catch(e){"
        "    var b=document.getElementById('cpb');"
        "    b.textContent='Copy failed';"
        "    setTimeout(function(){b.textContent='Copy Logs'},2000);"
        "  }"
        "  sel.removeAllRanges();"
        "}"
        "function trimLogs(){var p=document.getElementById('logs');while(p.childNodes.length>MAX_DOM)p.removeChild(p.firstChild);}"
        "function fetchLogs(){"
        "  var x=new XMLHttpRequest();"
        "  x.open('GET','/logs?from='+logIdx,true);"
        "  x.withCredentials=true;"
        "  x.onload=function(){"
        "    if(x.status!=200){console.log('logs err',x.status);return;}"
        "    var r=JSON.parse(x.responseText);"
        "    if(r.lines&&r.lines.length>0){"
        "      var p=document.getElementById('logs');"
        "      for(var i=0;i<r.lines.length;i++){p.appendChild(document.createTextNode(r.lines[i]+'\\n'));}"
        "      trimLogs();"
        "      p.scrollTop=p.scrollHeight;"
        "    }"
        "    logIdx=r.next_idx;"
        "  };"
        "  x.onerror=function(){console.log('logs XHR error');};"
        "  x.send();"
        "}"
        "fetchLogs();"
        "setInterval(fetchLogs,2000);"
        /* Metrics refresh every 10s */
        "function refreshMetrics(){"
        "  var x=new XMLHttpRequest();"
        "  x.open('GET','/metrics',true);"
        "  x.withCredentials=true;"
        "  x.onload=function(){"
        "    if(x.status!=200)return;"
        "    var m=JSON.parse(x.responseText);"
        "    var t=document.getElementById('mtbl');"
        "    if(!t)return;"
        "    var r=[];"
        "    r.push('<tr><th>Power State</th><td>'+(m.dyn_ac==1?'<span style=\\'color:#3f3\\'>ON LINE</span>':'<span style=\\'color:#f33\\'>ON BATTERY</span>')+'</td></tr>');"
        "    r.push('<tr><th>Load</th><td>'+m.dyn_load+'%</td></tr>');"
        "    r.push('<tr><th>Nominal/Flow (0x09)</th><td>'+m.dyn_nominal+'</td></tr>');"
        "    r.push('<tr><th>Est. Runtime</th><td>'+m.dyn_runtime+' min</td></tr>');"
        "    r.push('<tr><th>Battery Capacity</th><td>'+m.dyn_capacity+'%</td></tr>');"
        "    r.push('<tr><th>Time on Battery</th><td>'+m.dyn_time_on_batt+' min</td></tr>');"
        "    r.push('<tr><th>Charge Status (0x12)</th><td>'+m.dyn_charge_status+'</td></tr>');"
        "    r.push('<tr><th>Battery Health (0x14)</th><td>'+(m.dyn_replace_batt==3?'<span style=\\'color:#f33\\'>REPLACE</span>':(m.dyn_replace_batt==2?'<span style=\\'color:#3f3\\'>GOOD</span>':'Unknown'))+'</td></tr>');"
        "    t.innerHTML=r.join('');"
        "  };"
        "  x.send();"
        "}"
        "setInterval(refreshMetrics,10000);"
        /* Device Management Card */
        "function doReboot(){"
        "  if(!confirm('Reboot the device?'))return;"
        "  document.getElementById('rbt').textContent='Rebooting...';"
        "  var x=new XMLHttpRequest();"
        "  x.open('POST','/reboot',true);"
        "  x.onload=function(){setTimeout(function(){location.reload()},5000)};"
        "  x.onerror=function(){setTimeout(function(){location.reload()},5000)};"
        "  x.send();"
        "}"
        "function doUpdate(){"
        "  var f=document.getElementById('fwfile').files[0];"
        "  if(!f){alert('Select a firmware .bin file first');return;}"
        "  if(!confirm('Flash firmware: '+f.name+' ('+Math.round(f.size/1024)+'KB)?\\nDevice will reboot after upload.'))return;"
        "  document.getElementById('updbtn').textContent='Uploading...';document.getElementById('updbtn').disabled=true;"
        "  var x=new XMLHttpRequest();"
        "  x.open('POST','/update',true);"
        "  x.onload=function(){"
        "    if(x.status==200){document.getElementById('updbtn').textContent='Success! Rebooting...';setTimeout(function(){location.reload()},8000);}"
        "    else{document.getElementById('updbtn').textContent='Upload Failed';setTimeout(function(){document.getElementById('updbtn').textContent='Upload Firmware';document.getElementById('updbtn').disabled=false},3000);}"
        "  };"
        "  x.onerror=function(){document.getElementById('updbtn').textContent='Upload Error';setTimeout(function(){document.getElementById('updbtn').textContent='Upload Firmware';document.getElementById('updbtn').disabled=false},3000);};"
        "  x.upload.onprogress=function(e){if(e.lengthComputable){var pct=Math.round(e.loaded/e.total*100);document.getElementById('updbtn').textContent='Uploading '+pct+'%';}};"
        "  x.send(f);"
        "}"
        "function loadSystem(){"
        "  var x=new XMLHttpRequest();"
        "  x.open('GET','/system',true);"
        "  x.onload=function(){if(x.status!=200)return;var s=JSON.parse(x.responseText);var t=document.getElementById('systbl');if(!t)return;t.innerHTML='<tr><th>Firmware</th><td>'+s.firmware+'</td></tr><tr><th>Partition</th><td>'+s.running_partition+'</td></tr><tr><th>OTA Slot</th><td>'+(s.ota_slot>=0?s.ota_slot:'factory')+'</td></tr><tr><th>Free Heap</th><td>'+(s.free_heap/1024).toFixed(0)+' KB</td></tr><tr><th>Uptime</th><td>'+Math.floor(s.uptime_s/3600)+'h '+Math.floor((s.uptime_s%3600)/60)+'m</td></tr>';};"
        "  x.send();"
        "}"
        "loadSystem();"
        "</script>"
        "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ═══════════════ GET /logs — Incremental JSON Log Stream ═══════════════ */

static esp_err_t logs_handler(httpd_req_t *req)
{
    /* Parse ?from=N query param to get the last index the client has seen */
    int from_idx = 0;
    char query[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16] = {0};
        if (httpd_query_key_value(query, "from", val, sizeof(val)) == ESP_OK) {
            from_idx = atoi(val);
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (!log_mutex || !xSemaphoreTake(log_mutex, pdMS_TO_TICKS(100))) {
        httpd_resp_sendstr(req, "{\"lines\":[],\"next_idx\":0}");
        return ESP_OK;
    }

    /* Build JSON response with lines since from_idx */
    /* Track absolute position: log_write_idx is the next write slot */
    int global_next = (log_count < LOG_RING_SIZE) ? log_count : (log_count);
    /* global_next is the absolute index the NEXT write will get */

    /* Figure out which lines to send: everything from from_idx onward */
    int start_line = from_idx;
    if (start_line < 0) start_line = 0;
    if (start_line > global_next) start_line = global_next;

    /* Clamp to ring size and limit per response (max 100 lines per poll) */
    int available = global_next - start_line;
    if (available > LOG_RING_SIZE) {
        start_line = global_next - LOG_RING_SIZE;
        available = LOG_RING_SIZE;
    }
    const int MAX_LINES_PER_RESPONSE = 100;
    if (available > MAX_LINES_PER_RESPONSE) {
        available = MAX_LINES_PER_RESPONSE;
    }
    int next_idx = start_line + available;

    httpd_resp_sendstr_chunk(req, "{\"lines\":[");

    bool first = true;
    for (int i = 0; i < available; i++) {
        int abs_pos = start_line + i;
        int ring_idx = abs_pos % LOG_RING_SIZE;

        if (log_ring[ring_idx][0] != '\0') {
            if (!first) httpd_resp_sendstr_chunk(req, ",");
            httpd_resp_sendstr_chunk(req, "\"");

            /* JSON-escape the line */
            const char *s = log_ring[ring_idx];
            while (*s) {
                switch (*s) {
                    case '"':  httpd_resp_sendstr_chunk(req, "\\\""); break;
                    case '\\': httpd_resp_sendstr_chunk(req, "\\\\"); break;
                    case '\n': httpd_resp_sendstr_chunk(req, "\\n");  break;
                    case '\r': break;  /* skip CR */
                    case '\t': httpd_resp_sendstr_chunk(req, "\\t");  break;
                    default:
                        if ((unsigned char)*s < 0x20) {
                            char ubuf[8];
                            snprintf(ubuf, sizeof(ubuf), "\\u%04x", (unsigned char)*s);
                            httpd_resp_sendstr_chunk(req, ubuf);
                        } else {
                            char c[2] = {*s, 0};
                            httpd_resp_sendstr_chunk(req, c);
                        }
                }
                s++;
            }
            httpd_resp_sendstr_chunk(req, "\"");
            first = false;
        }
    }

    char tail[64];
    snprintf(tail, sizeof(tail), "],\"next_idx\":%d}", next_idx);
    httpd_resp_sendstr_chunk(req, tail);

    xSemaphoreGive(log_mutex);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ═══════════════ GET /metrics — JSON UPS Metrics ═══════════════ */

static esp_err_t metrics_handler(httpd_req_t *req)
{
    if (!check_basic_auth(req)) return ESP_OK;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    ups_metrics_t metrics_snapshot = apc_hid_get_metrics_snapshot();
    const ups_metrics_t *m = &metrics_snapshot;
    ups_transport_stats_t stats = ups_transport_stats_get();
    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t last_poll_age_ms = (stats.last_poll_ms > 0 && now_ms >= stats.last_poll_ms) ? (now_ms - stats.last_poll_ms) : 0;
    const char *transport_health = (stats.get_report_error || stats.get_report_timeout) ? "DEGRADED" : "OK";

    char buf[768];
    snprintf(buf, 1024,
        "{\"dyn_ac\":%d,\"dyn_load\":%.1f,\"dyn_nominal\":%d,\"dyn_runtime\":%d,"
        "\"dyn_capacity\":%d,\"dyn_time_on_batt\":%d,\"dyn_charge_status\":%d,\"dyn_replace_batt\":%d,"
        "\"status\":\"%s\",\"status_class\":\"%s\",\"status_confidence\":\"%s\","
        "\"active_profile\":\"%s\",\"transport_health\":\"%s\",\"last_poll_age_ms\":%lu,"
        "\"poll_cycles\":%lu,\"get_report_success\":%lu,\"get_report_timeout\":%lu,\"get_report_stall\":%lu,\"get_report_error\":%lu,"
        "\"charge\":%.0f,\"voltage\":%.1f,\"runtime\":%.0f,\"runtime_min\":%.1f,"
        "\"input_voltage\":%.0f,\"load\":%.0f,"
        "\"nominal_power\":%.0f,\"input_nominal\":%.0f,\"beeper\":\"%s\"}",
        m->dynamic_ac_present, m->dynamic_load_percent, m->dynamic_nominal_flow, m->dynamic_runtime_min,
        m->dynamic_battery_capacity, m->dynamic_time_on_battery_min, m->dynamic_charge_status, m->dynamic_replace_battery,
        m->status_string,
        m->status.online ? "online" : "offline",
        m->status_confidence,
        ups_profile_name(apc_hid_parser_get_profile()),
        transport_health,
        (unsigned long)last_poll_age_ms,
        (unsigned long)stats.poll_cycles_completed,
        (unsigned long)stats.get_report_success,
        (unsigned long)stats.get_report_timeout,
        (unsigned long)stats.get_report_stall,
        (unsigned long)stats.get_report_error,
        m->battery_charge,
        m->battery_voltage,
        m->battery_runtime, m->battery_runtime / 60.0f,
        m->input_voltage, m->load_percent,
        m->nominal_power,
        m->input_voltage_nominal,
        m->beeper_status);

    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ═══════════════ GET /version — Public Firmware Version ═══════════════ */

static esp_err_t version_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char buf[192];
    snprintf(buf, 1024,
             "{\"name\":\"UPS MQTT Bridge\",\"version\":\"%s\",\"chipFamily\":\"ESP32-S3\"}\n",
             FW_VERSION);
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ═══════════════ POST /save — Save Config & Reboot ═══════════════ */

static esp_err_t save_handler(httpd_req_t *req)
{
    if (!check_basic_auth(req)) return ESP_OK;

    char body[1024];
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data received");
        return ESP_FAIL;
    }
    body[recv_len] = '\0';

    app_config_t new_config = *current_config;
    char val[128];

    if (get_form_value(body, "wifi_ssid", val, sizeof(val)))
        strlcpy(new_config.wifi_ssid, val, sizeof(new_config.wifi_ssid));
    if (get_form_value(body, "wifi_pass", val, sizeof(val)))
        strlcpy(new_config.wifi_pass, val, sizeof(new_config.wifi_pass));
    if (get_form_value(body, "mqtt_url", val, sizeof(val)))
        strlcpy(new_config.mqtt_url, val, sizeof(new_config.mqtt_url));
    if (get_form_value(body, "mqtt_user", val, sizeof(val)))
        strlcpy(new_config.mqtt_user, val, sizeof(new_config.mqtt_user));
    if (get_form_value(body, "mqtt_pass", val, sizeof(val)))
        strlcpy(new_config.mqtt_pass, val, sizeof(new_config.mqtt_pass));
    if ((get_form_value(body, "admin_pass", val, sizeof(val)) ||
         get_form_value(body, "web_pass", val, sizeof(val))) && val[0] != '\0') {
        if (strlen(val) < 8 || strlen(val) >= sizeof(new_config.web_pass)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Config AP/admin password must be 8-63 characters");
            return ESP_FAIL;
        }
        strlcpy(new_config.provisioning_pass, val, sizeof(new_config.provisioning_pass));
        strlcpy(new_config.web_pass, val, sizeof(new_config.web_pass));
    }
    if (get_form_value(body, "device_label", val, sizeof(val)))
        strlcpy(new_config.device_label, val, sizeof(new_config.device_label));
    if (get_form_value(body, "ups_profile", val, sizeof(val))) {
        int profile = atoi(val);
        if (profile >= UPS_PROFILE_AUTO && profile <= UPS_PROFILE_APC_GENERIC_HID) {
            new_config.ups_profile = (uint8_t)profile;
        }
    }
    if (get_form_value(body, "interval", val, sizeof(val))) {
        int secs = atoi(val);
        if (secs >= 5 && secs <= 300)
            new_config.publish_interval_ms = (uint32_t)secs * 1000;
    }

    esp_err_t err = config_save(&new_config);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Config saved to NVS, rebooting...");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>");
    httpd_resp_sendstr_chunk(req, PAGE_STYLE);
    httpd_resp_sendstr_chunk(req,
        "</style></head><body>"
        "<h1>Config Saved!</h1>"
        "<p style='text-align:center'>Stored in NVS. WiFi and MQTT credentials are not compiled into firmware.</p>"
        "<p style='text-align:center'>Rebooting in 2 seconds...</p>"
        "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;
}


/* ═══════════════ USB Debug UI/API ═══════════════ */

static const char *debug_mode_name(usb_debug_mode_t mode)
{
    switch (mode) {
        case USB_DEBUG_MODE_PASSIVE: return "passive";
        case USB_DEBUG_MODE_ACTIVE: return "active";
        case USB_DEBUG_MODE_OFF:
        default: return "off";
    }
}

static esp_err_t recv_small_body(httpd_req_t *req, char *body, size_t body_size)
{
    if (!body || body_size == 0) return ESP_ERR_INVALID_ARG;
    size_t remaining = req->content_len;
    if (remaining >= body_size) remaining = body_size - 1;
    size_t off = 0;
    while (remaining > 0) {
        int r = httpd_req_recv(req, body + off, remaining);
        if (r <= 0) return ESP_FAIL;
        off += r;
        remaining -= r;
    }
    body[off] = '\0';
    return ESP_OK;
}

static uint32_t parse_u32_auto(const char *s)
{
    return s ? (uint32_t)strtoul(s, NULL, 0) : 0;
}

static esp_err_t usb_debug_page_handler(httpd_req_t *req)
{
    if (!check_basic_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    usb_debug_config_t cfg;
    usb_debug_get_config(&cfg);
    char buf[512];

    send_page_header(req, "USB Debug", false);
    httpd_resp_sendstr_chunk(req,
        "<div class='card'><h2>USB HID Debugger</h2>"
        "<p>Keeps Wi-Fi, HTTP, logs, and Web OTA alive. Active Debug pauses normal bridge parsing/polling so raw USB commands do not compete with MQTT telemetry.</p>"
        "<p><strong>Runtime only:</strong> debug mode is not saved to NVS. Reboot always returns to Normal Bridge mode.</p>"
        "<form method='POST' action='/api/usb-debug/config'>"
        "<label>Mode</label><select name='mode'>");

    snprintf(buf, 1024,
        "<option value='off' %s>Normal Bridge</option>"
        "<option value='passive' %s>Passive Capture</option>"
        "<option value='active' %s>Active Debug</option>",
        cfg.mode == USB_DEBUG_MODE_OFF ? "selected" : "",
        cfg.mode == USB_DEBUG_MODE_PASSIVE ? "selected" : "",
        cfg.mode == USB_DEBUG_MODE_ACTIVE ? "selected" : "");
    httpd_resp_sendstr_chunk(req, buf);

    httpd_resp_sendstr_chunk(req,
        "</select>"
        "<div style='display:grid;grid-template-columns:1.5rem 1fr;gap:.45rem .75rem;align-items:start;margin:.75rem 0;'>");

    snprintf(buf, 1024,
        "<input id='cap_int' type='checkbox' name='cap_int' value='1' %s><label for='cap_int'>Capture interrupt-IN reports</label>",
        cfg.capture_interrupt_reports ? "checked" : "");
    httpd_resp_sendstr_chunk(req, buf);

    snprintf(buf, 1024,
        "<input id='cap_feat' type='checkbox' name='cap_feat' value='1' %s><label for='cap_feat'>Capture normal GET_REPORT polls</label>",
        cfg.capture_feature_reports ? "checked" : "");
    httpd_resp_sendstr_chunk(req, buf);

    snprintf(buf, 1024,
        "<input id='raw_setup' type='checkbox' name='raw_setup' value='1' %s><label for='raw_setup'>Include raw 8-byte USB control SETUP packet in descriptor dumps</label>",
        cfg.include_control_setup ? "checked" : "");
    httpd_resp_sendstr_chunk(req, buf);

    snprintf(buf, 1024,
        "<input id='log' type='checkbox' name='log' value='1' %s><label for='log'>Mirror debug records to ESP log</label>",
        cfg.log_to_esp_log ? "checked" : "");
    httpd_resp_sendstr_chunk(req, buf);

    httpd_resp_sendstr_chunk(req,
        "</div>"
        "<button type='submit'>Apply Debug Mode</button></form>"
        "<form method='POST' action='/api/usb-debug/descriptor'><button type='submit'>Dump HID Report Descriptor</button></form>"
        "<form method='POST' action='/api/usb-debug/request-safe'>"
        "<label>Report Type (1=input, 2=output, 3=feature)</label><input name='type' value='3'>"
        "<label>Report ID (hex OK, e.g. 0x0D)</label><input name='id' value='0x0D'>"
        "<label>Length</label><input name='len' value='64'>"
        "<button type='submit'>Safe GET_REPORT</button><small> Uses descriptor-sized guarded reads; raw endpoint remains /api/usb-debug/request.</small></form>"
        "<form method='POST' action='/api/usb-debug/clear'><button type='submit'>Clear Debug Capture</button></form>"
        "<p><a href='/api/usb-debug'>State JSON</a> | <a href='/api/usb-debug/records'>Captured Records</a> | <a href='/api/usb-debug/records.json'>Records JSON</a></p>"
        "<pre id='dbg'>Loading...</pre>"
        "<script>async function poll(){try{let s=await fetch('/api/usb-debug');let r=await fetch('/api/usb-debug/records');document.getElementById('dbg').textContent=await s.text()+'\\n\\n'+await r.text();}catch(e){document.getElementById('dbg').textContent=e;}setTimeout(poll,2000);}poll();</script>"
        "</div>");
    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t usb_debug_state_handler(httpd_req_t *req)
{
    if (!check_basic_auth(req)) return ESP_OK;
    usb_debug_state_t st; usb_debug_config_t cfg;
    usb_debug_get_state(&st); usb_debug_get_config(&cfg);
    char buf[512];
    snprintf(buf, 1024,
        "{\"mode\":\"%s\",\"ups_connected\":%s,\"capture_interrupt\":%s,\"capture_feature\":%s,\"include_control_setup\":%s,\"log_to_esp_log\":%s,\"interrupt_reports_seen\":%lu,\"feature_reports_seen\":%lu,\"descriptor_dumps\":%lu,\"errors\":%lu,\"dropped_records\":%lu,\"last_activity_ms\":%lld}",
        debug_mode_name(cfg.mode), st.ups_connected ? "true" : "false", cfg.capture_interrupt_reports ? "true" : "false", cfg.capture_feature_reports ? "true" : "false", cfg.include_control_setup ? "true" : "false", cfg.log_to_esp_log ? "true" : "false", (unsigned long)st.interrupt_reports_seen, (unsigned long)st.feature_reports_seen, (unsigned long)st.descriptor_dumps, (unsigned long)st.errors, (unsigned long)st.dropped_records, (long long)st.last_activity_ms);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    return httpd_resp_sendstr(req, buf);
}

static uint32_t get_query_u32(httpd_req_t *req, const char *key, uint32_t def)
{
    char query[128];
    char val[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return def;
    if (httpd_query_key_value(query, key, val, sizeof(val)) != ESP_OK) return def;
    return (uint32_t)parse_u32_auto(val);
}

static esp_err_t usb_debug_records_handler(httpd_req_t *req)
{
    if (!check_basic_auth(req)) return ESP_OK;
    uint32_t since = get_query_u32(req, "since", 0);
    usb_debug_record_t recs[16];
    size_t n = usb_debug_get_records(recs, 16, since);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    char line[384];
    for (size_t i = 0; i < n; i++) {
        char hex[USB_DEBUG_MAX_RECORD_DATA * 3 + 1] = {0}; int pos = 0;
        size_t data_len = recs[i].length > USB_DEBUG_MAX_RECORD_DATA ? USB_DEBUG_MAX_RECORD_DATA : recs[i].length;
        for (size_t b = 0; b < data_len && pos < (int)sizeof(hex) - 4; b++) pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", recs[i].data[b]);
        snprintf(line, sizeof(line), "%lu %lldms type=%d report=0x%02X status=%u len=%u note=%s data=%s\n", (unsigned long)recs[i].seq, (long long)recs[i].timestamp_ms, recs[i].type, recs[i].report_id, recs[i].status, recs[i].length, recs[i].note, hex);
        httpd_resp_sendstr_chunk(req, line);
    }
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t usb_debug_records_json_handler(httpd_req_t *req)
{
    if (!check_basic_auth(req)) return ESP_OK;
    uint32_t since = get_query_u32(req, "since", 0);
    usb_debug_record_t recs[16];
    size_t n = usb_debug_get_records(recs, 16, since);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_sendstr_chunk(req, "[\n");
    char line[384];
    for (size_t i = 0; i < n; i++) {
        char hex[USB_DEBUG_MAX_RECORD_DATA * 3 + 1] = {0};
        int pos = 0;
        size_t data_len = recs[i].length > USB_DEBUG_MAX_RECORD_DATA ? USB_DEBUG_MAX_RECORD_DATA : recs[i].length;
        for (size_t b = 0; b < data_len && pos < (int)sizeof(hex) - 4; b++) {
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%s%02X", b ? " " : "", recs[i].data[b]);
        }
        // Notes are controlled firmware strings; keep them short and replace quotes defensively.
        char note[96];
        size_t ni = 0;
        for (size_t j = 0; recs[i].note[j] && ni < sizeof(note) - 1; j++) {
            note[ni++] = (recs[i].note[j] == '"' || recs[i].note[j] == '\\') ? '_' : recs[i].note[j];
        }
        note[ni] = 0;
        snprintf(line, sizeof(line),
            "%s{\"seq\":%lu,\"ms\":%lld,\"type\":%d,\"report\":\"0x%02X\",\"status\":%u,\"len\":%u,\"note\":\"%s\",\"data\":\"%s\"}\n",
            i ? "," : "", (unsigned long)recs[i].seq, (long long)recs[i].timestamp_ms,
            recs[i].type, recs[i].report_id, recs[i].status, recs[i].length, note, hex);
        httpd_resp_sendstr_chunk(req, line);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t usb_debug_config_handler(httpd_req_t *req)
{
    if (!check_basic_auth(req)) return ESP_OK;
    char body[256];
    if (recv_small_body(req, body, sizeof(body)) != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    char mode[24] = "off"; get_form_value(body, "mode", mode, sizeof(mode));
    usb_debug_config_t cfg = {0};
    if (strcmp(mode, "passive") == 0) cfg.mode = USB_DEBUG_MODE_PASSIVE;
    else if (strcmp(mode, "active") == 0) cfg.mode = USB_DEBUG_MODE_ACTIVE;
    else cfg.mode = USB_DEBUG_MODE_OFF;
    cfg.capture_interrupt_reports = strstr(body, "cap_int=") != NULL;
    cfg.capture_feature_reports = strstr(body, "cap_feat=") != NULL;
    cfg.include_control_setup = strstr(body, "raw_setup=") != NULL;
    cfg.log_to_esp_log = strstr(body, "log=") != NULL;
    esp_err_t err = usb_debug_set_config(&cfg);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    httpd_resp_set_status(req, "303 See Other"); httpd_resp_set_hdr(req, "Location", "/usb-debug"); return httpd_resp_sendstr(req, "");
}

static esp_err_t usb_debug_descriptor_handler(httpd_req_t *req)
{
    if (!check_basic_auth(req)) return ESP_OK;

    usb_debug_config_t cfg;
    usb_debug_get_config(&cfg);
    if (cfg.mode != USB_DEBUG_MODE_ACTIVE) {
        cfg.mode = USB_DEBUG_MODE_ACTIVE;
        esp_err_t cfg_err = usb_debug_set_config(&cfg);
        if (cfg_err != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(cfg_err));
        }
    }

    esp_err_t err = usb_debug_request_descriptor();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "descriptor queued; return to /usb-debug and refresh Records JSON in a few seconds\\n");
}

static esp_err_t usb_debug_request_common_handler(httpd_req_t *req, bool safe)
{
    if (!check_basic_auth(req)) return ESP_OK;
    char body[256], val[32];
    if (recv_small_body(req, body, sizeof(body)) != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    uint8_t type = 3, id = 0; uint16_t len = 64;
    if (get_form_value(body, "type", val, sizeof(val))) type = (uint8_t)parse_u32_auto(val);
    if (get_form_value(body, "id", val, sizeof(val))) id = (uint8_t)parse_u32_auto(val);
    if (get_form_value(body, "len", val, sizeof(val))) len = (uint16_t)parse_u32_auto(val);
    esp_err_t err = safe ? usb_debug_request_report_safe(type, id, len) : usb_debug_request_report(type, id, len);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
    httpd_resp_set_status(req, "303 See Other"); httpd_resp_set_hdr(req, "Location", "/usb-debug"); return httpd_resp_sendstr(req, "");
}

static esp_err_t usb_debug_request_handler(httpd_req_t *req)
{
    return usb_debug_request_common_handler(req, false);
}

static esp_err_t usb_debug_request_safe_handler(httpd_req_t *req)
{
    return usb_debug_request_common_handler(req, true);
}

static esp_err_t usb_debug_clear_handler(httpd_req_t *req)
{
    if (!check_basic_auth(req)) return ESP_OK;
    usb_debug_clear_records();
    httpd_resp_set_status(req, "303 See Other"); httpd_resp_set_hdr(req, "Location", "/usb-debug"); return httpd_resp_sendstr(req, "");
}

/* ═══════════════ Server Start ═══════════════ */

static httpd_handle_t server = NULL;

esp_err_t http_server_start(app_config_t *config)
{
    current_config = config;

    /* Start log capture -- small bridge buffer, DOM is the real buffer */
    log_mutex = xSemaphoreCreateMutex();
    if (log_mutex) {
        original_vprintf_fn = esp_log_set_vprintf(capture_vprintf);
        ESP_LOGI(TAG, "Log capture enabled (%d line bridge buffer, DOM is real buffer)", LOG_RING_SIZE);
    }

    httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
    httpd_config.stack_size = 8192;
    httpd_config.max_uri_handlers = 20;

    esp_err_t err = httpd_start(&server, &httpd_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t root_uri    = { .uri = "/",        .method = HTTP_GET,  .handler = root_handler    };
    const httpd_uri_t status_uri  = { .uri = "/status",   .method = HTTP_GET,  .handler = status_handler  };
    const httpd_uri_t logs_uri    = { .uri = "/logs",     .method = HTTP_GET,  .handler = logs_handler    };
    const httpd_uri_t metrics_uri = { .uri = "/metrics",  .method = HTTP_GET,  .handler = metrics_handler };
    const httpd_uri_t version_uri = { .uri = "/version",  .method = HTTP_GET,  .handler = version_handler };
    const httpd_uri_t save_uri    = { .uri = "/save",     .method = HTTP_POST, .handler = save_handler    };
    const httpd_uri_t usb_debug_page_uri = { .uri = "/usb-debug", .method = HTTP_GET, .handler = usb_debug_page_handler };
    const httpd_uri_t usb_debug_state_uri = { .uri = "/api/usb-debug", .method = HTTP_GET, .handler = usb_debug_state_handler };
    const httpd_uri_t usb_debug_records_uri = { .uri = "/api/usb-debug/records", .method = HTTP_GET, .handler = usb_debug_records_handler };
    const httpd_uri_t usb_debug_records_json_uri = { .uri = "/api/usb-debug/records.json", .method = HTTP_GET, .handler = usb_debug_records_json_handler };
    const httpd_uri_t usb_debug_config_uri = { .uri = "/api/usb-debug/config", .method = HTTP_POST, .handler = usb_debug_config_handler };
    const httpd_uri_t usb_debug_descriptor_uri = { .uri = "/api/usb-debug/descriptor", .method = HTTP_POST, .handler = usb_debug_descriptor_handler };
    const httpd_uri_t usb_debug_request_uri = { .uri = "/api/usb-debug/request", .method = HTTP_POST, .handler = usb_debug_request_handler };
    const httpd_uri_t usb_debug_request_safe_uri = { .uri = "/api/usb-debug/request-safe", .method = HTTP_POST, .handler = usb_debug_request_safe_handler };
    const httpd_uri_t usb_debug_clear_uri = { .uri = "/api/usb-debug/clear", .method = HTTP_POST, .handler = usb_debug_clear_handler };

    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &logs_uri);
    httpd_register_uri_handler(server, &metrics_uri);
    httpd_register_uri_handler(server, &version_uri);
    httpd_register_uri_handler(server, &save_uri);
    httpd_register_uri_handler(server, &usb_debug_page_uri);
    httpd_register_uri_handler(server, &usb_debug_state_uri);
    httpd_register_uri_handler(server, &usb_debug_records_uri);
    httpd_register_uri_handler(server, &usb_debug_records_json_uri);
    httpd_register_uri_handler(server, &usb_debug_config_uri);
    httpd_register_uri_handler(server, &usb_debug_descriptor_uri);
    httpd_register_uri_handler(server, &usb_debug_request_uri);
    httpd_register_uri_handler(server, &usb_debug_request_safe_uri);
    httpd_register_uri_handler(server, &usb_debug_clear_uri);

    /* OTA & system management endpoints */
    register_ota_handlers(server);

    ESP_LOGI(TAG, "HTTP server started on port %d", httpd_config.server_port);
    return ESP_OK;
}
