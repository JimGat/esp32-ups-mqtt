/*
 * ═══════════════════════════════════════════════════════════════════════════
 * APC UPS USB HOST MANAGER - Comprehensive Architecture Overview
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * PURPOSE:
 * This module handles USB communication with an APC Back-UPS via USB HID protocol.
 * The ESP32-S3 acts as a USB HOST (like your computer), and the UPS acts as a
 * USB DEVICE (like a keyboard or mouse).
 *
 * WHY TWO TYPES OF USB TRANSFERS?
 * ─────────────────────────────────────────────────────────────────────────
 * 1. INTERRUPT TRANSFERS (Automatic, pushed by UPS):
 *    - The UPS automatically sends status updates every ~200-2000ms
 *    - These contain: battery charge, runtime, status bits (online/charging)
 *    - Think of this like the UPS "tapping you on the shoulder" with updates
 *    - Example reports: 0x06 (status), 0x0C (charge+runtime), 0x16 (detailed status)
 *
 * 2. CONTROL TRANSFERS / GET_REPORT (On-demand, we ask the UPS):
 *    - We must actively REQUEST certain data by asking for specific "Feature Reports"
 *    - These contain: voltage, load percentage, transfer thresholds
 *    - Think of this like us "asking a question" and waiting for the answer
 *    - Example reports: 0x09 (battery voltage), 0x31 (input voltage), 0x50 (load %)
 *
 * WHY DOESN'T THE UPS SEND EVERYTHING VIA INTERRUPTS?
 * ─────────────────────────────────────────────────────────────────────────
 * - USB HID devices categorize data into "Input Reports" (pushed) and
 *   "Feature Reports" (polled on request)
 * - Status data that changes frequently → Input Reports (interrupt transfers)
 * - Configuration/slow-changing data → Feature Reports (control transfers)
 * - This is standard HID behavior, not specific to APC
 *
 * THE CALLBACK MYSTERY - WHY WAS IT SO HARD?
 * ─────────────────────────────────────────────────────────────────────────
 * In ESP-IDF, USB transfers complete asynchronously via CALLBACKS:
 * - When you submit a transfer, it returns immediately (non-blocking)
 * - Later, when data arrives, a callback function fires
 * - BUT: Callbacks only fire when you call usb_host_xxx_handle_events()
 *
 * TWO LEVELS OF EVENT HANDLING (This was the key breakthrough!):
 * 1. usb_host_lib_handle_events()    - Library level (device connections, control transfers)
 * 2. usb_host_client_handle_events() - Client level (transfer completion callbacks)
 *
 * FOR INTERRUPT TRANSFERS: Only client events needed
 * FOR CONTROL TRANSFERS: BOTH lib AND client events needed
 *
 * This is why GET_REPORT was timing out - we weren't processing library events!
 *
 * THREAD SAFETY:
 * ─────────────────────────────────────────────────────────────────────────
 * - transfer_mutex: Prevents interrupt and control transfers from running simultaneously
 * - transfer_done: Semaphore to signal when a transfer callback has fired
 *
 * DATA FLOW:
 * ─────────────────────────────────────────────────────────────────────────
 * USB Device → Interrupt Transfer → Raw HID Report (bytes) →
 * apc_hid_parser.c (decode) → ups_metrics_t struct →
 * main.c (MQTT publish) → Home Assistant
 *
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include "usb_host_manager.h"
#include "apc_hid_parser.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"
#include <string.h>

static const char *TAG = "usb_host";

/* ---- Evil Hardware Hacker Mode: Dump HID Report Descriptor ---- */
static void hid_report_desc_cb(usb_transfer_t *transfer) {
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGI(TAG, "=== HID REPORT DESCRIPTOR (%d bytes) ===", transfer->actual_num_bytes);
        for (int i = 0; i < transfer->actual_num_bytes; i += 16) {
            char line[80];
            int offset = 0;
            for (int j = 0; j < 16; j++) {
                if (i + j < transfer->actual_num_bytes) {
                    offset += sprintf(&line[offset], "%02x ", transfer->data_buffer[i + j]);
                } else {
                    offset += sprintf(&line[offset], "   ");
                }
            }
            ESP_LOGI(TAG, "%04x: %s", i, line);
        }
    } else {
        ESP_LOGE(TAG, "Failed to get HID Report Descriptor: %d", transfer->status);
    }
    usb_host_transfer_free(transfer);
}

static void request_hid_report_descriptor(usb_device_handle_t dev_hdl, uint8_t intf_num) {
    usb_transfer_t *ctrl_xfer = NULL;
    size_t xfer_size = sizeof(usb_setup_packet_t) + 512;
    
    esp_err_t err = usb_host_transfer_alloc(xfer_size, 0, &ctrl_xfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to alloc ctrl xfer: %s", esp_err_to_name(err));
        return;
    }

    ctrl_xfer->device_handle = dev_hdl;
    ctrl_xfer->bEndpointAddress = 0;
    ctrl_xfer->callback = hid_report_desc_cb;
    ctrl_xfer->context = NULL;
    ctrl_xfer->timeout_ms = 2000;
    ctrl_xfer->num_bytes = xfer_size;

    usb_setup_packet_t *setup = (usb_setup_packet_t *)ctrl_xfer->data_buffer;
    setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    setup->bRequest = USB_B_REQUEST_GET_DESCRIPTOR;
    setup->wValue = (0x22 << 8) | 0; // 0x22 = HID Report Descriptor, Index 0
    setup->wIndex = intf_num;
    setup->wLength = 512;

    err = usb_host_transfer_submit(ctrl_xfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to submit ctrl xfer: %s", esp_err_to_name(err));
        usb_host_transfer_free(ctrl_xfer);
    } else {
        ESP_LOGI(TAG, "🔍 Requesting HID Report Descriptor from UPS...");
    }
}
/* ----------------------------------------------------------------------- */

//══════════════════════════════════════════════════════════════════════════════
// USB DEVICE IDENTIFICATION
//══════════════════════════════════════════════════════════════════════════════
// APC UPS USB Vendor/Product IDs (identifies this specific UPS model)
// VID 0x051D = American Power Conversion
// PID 0x0002 = Back-UPS series (Back-UPS XS 1000M, etc.)
// PID 0x0003 = Smart-UPS series (Smart-UPS C 1500, etc.)
#define APC_VID 0x051d
#define APC_PID_BACKUPS  0x0002
#define APC_PID_SMARTUPS 0x0003
#define IS_APC_UPS(vid, pid) ((vid) == APC_VID && ((pid) == APC_PID_BACKUPS || (pid) == APC_PID_SMARTUPS))

//══════════════════════════════════════════════════════════════════════════════
// USB HOST STATE TRACKING
//══════════════════════════════════════════════════════════════════════════════
// These variables track the current state of the USB connection
static bool ups_connected = false;           // Is UPS physically connected?
static bool hid_report_desc_requested = false; // Flag to request HID Report Descriptor from main task
static SemaphoreHandle_t usb_mutex = NULL;   // Mutex for USB library access
static usb_host_client_handle_t usb_client = NULL;  // Our USB client handle
static usb_device_handle_t ups_device = NULL;       // Handle to the UPS device

//══════════════════════════════════════════════════════════════════════════════
// HID (Human Interface Device) CONFIGURATION
//══════════════════════════════════════════════════════════════════════════════
// HID Interface: UPS uses interface 0 for all HID communication
#define HID_INTERFACE 0

// HID Interrupt Endpoint: 0x81 means IN endpoint 1 (device-to-host)
// This is where the UPS automatically sends status updates
#define HID_INTERRUPT_IN_EP 0x81

// USB Host client event handler
static void usb_host_client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    ESP_LOGI(TAG, "DEBUG: Event callback triggered, event=%d", event_msg->event);

    switch (event_msg->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV:
            ESP_LOGI(TAG, "🆕 New USB device detected (addr=%d)", event_msg->new_dev.address);

            // Open the device
            usb_device_handle_t dev_hdl;
            esp_err_t err = usb_host_device_open(usb_client, event_msg->new_dev.address, &dev_hdl);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to open device: %s", esp_err_to_name(err));
                break;
            }

            // Get device descriptor
            const usb_device_desc_t *dev_desc;
            err = usb_host_get_device_descriptor(dev_hdl, &dev_desc);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to get device descriptor: %s", esp_err_to_name(err));
                usb_host_device_close(usb_client, dev_hdl);
                break;
            }

            ESP_LOGI(TAG, "DEBUG: Device VID:PID = %04X:%04X", dev_desc->idVendor, dev_desc->idProduct);

            // Check if this is our APC UPS
            if (IS_APC_UPS(dev_desc->idVendor, dev_desc->idProduct)) {
                ESP_LOGI(TAG, "🔌 APC UPS found! VID:PID = %04X:%04X",
                         dev_desc->idVendor, dev_desc->idProduct);

                ups_device = dev_hdl;
                ups_connected = true;

                // Claim HID interface FIRST (before inspecting)
                err = usb_host_interface_claim(usb_client, ups_device, HID_INTERFACE, 0);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to claim interface: %s", esp_err_to_name(err));
                } else {
                    ESP_LOGI(TAG, "✅ HID interface claimed successfully");
                    // Flag to request HID Report Descriptor from the main task (safe context)
                    hid_report_desc_requested = true;
                }

                // Get configuration descriptor to inspect endpoints (after claiming)
                const usb_config_desc_t *config_desc;
                err = usb_host_get_active_config_descriptor(dev_hdl, &config_desc);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "📋 Config: %d interfaces", config_desc->bNumInterfaces);

                    // Parse interfaces and endpoints (don't claim again!)
                    int offset = 0;
                    const usb_intf_desc_t *intf = usb_parse_interface_descriptor(config_desc, HID_INTERFACE, 0, &offset);
                    if (intf) {
                        ESP_LOGI(TAG, "  Interface %d: class=0x%02X, endpoints=%d",
                                 HID_INTERFACE, intf->bInterfaceClass, intf->bNumEndpoints);

                        // Log endpoints
                        int ep_offset = offset;
                        for (int e = 0; e < intf->bNumEndpoints; e++) {
                            const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(intf, e, config_desc->wTotalLength, &ep_offset);
                            if (ep) {
                                ESP_LOGI(TAG, "    Endpoint 0x%02X: type=%d, maxPacket=%d",
                                         ep->bEndpointAddress,
                                         ep->bmAttributes & 0x03,
                                         ep->wMaxPacketSize);
                            }
                        }
                    }
                }
            } else {
                ESP_LOGI(TAG, "⚠️ Not an APC UPS (VID:PID = %04X:%04X), expected VID=%04X",
                         dev_desc->idVendor, dev_desc->idProduct, APC_VID);
                usb_host_device_close(usb_client, dev_hdl);
            }
            break;

        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            ESP_LOGW(TAG, "🚫 USB device removed");
            if (event_msg->dev_gone.dev_hdl == ups_device) {
                ups_connected = false;
                ups_device = NULL;
                ESP_LOGI(TAG, "❌ APC UPS disconnected");
            }
            break;

        default:
            ESP_LOGI(TAG, "DEBUG: Unknown event %d", event_msg->event);
            break;
    }
}

//══════════════════════════════════════════════════════════════════════════════
// THREAD SYNCHRONIZATION FOR USB TRANSFERS
//══════════════════════════════════════════════════════════════════════════════

// transfer_done: Binary semaphore to signal when a USB transfer callback fires
// - We submit a transfer (returns immediately)
// - We wait on this semaphore
// - When data arrives, callback fires and gives this semaphore
// - We wake up and process the data
static SemaphoreHandle_t transfer_done;

// transfer_mutex: Prevents simultaneous interrupt + control transfers
// WHY NEEDED: The USB hardware can only handle one transfer at a time per endpoint
// - Interrupt transfers use endpoint 0x81
// - Control transfers use endpoint 0x00
// - But they share internal USB resources, so we serialize them
static SemaphoreHandle_t transfer_mutex;

//══════════════════════════════════════════════════════════════════════════════
// USB TRANSFER COMPLETION CALLBACK
//══════════════════════════════════════════════════════════════════════════════
// This function is called by the USB driver when a transfer completes
// CRITICAL: This runs in interrupt context, so keep it FAST and minimal
// - Just signal the semaphore
// - Don't do heavy processing here
// - Let the main task wake up and handle the data
static void transfer_callback(usb_transfer_t *transfer)
{
    // Signal that transfer is complete by "giving" the semaphore
    // The waiting task will wake up when it tries to "take" this semaphore
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(transfer_done, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

//══════════════════════════════════════════════════════════════════════════════
// GET_REPORT: REQUEST FEATURE REPORTS FROM THE UPS
//══════════════════════════════════════════════════════════════════════════════
// This function actively ASKS the UPS for specific data using HID GET_REPORT
//
// USB HID GET_REPORT Protocol:
// ────────────────────────────────────────────────────────────────────────────
// 1. We send a control transfer with:
//    - bmRequestType: 0xA1 (Device-to-Host, Class request, Interface recipient)
//    - bRequest: 0x01 (GET_REPORT - standard HID request)
//    - wValue: (ReportType << 8) | ReportID
//      * ReportType = 3 (Feature Report) - polled data like voltage, load
//      * ReportID = specific report we want (0x09=battery voltage, 0x31=input voltage, etc.)
//    - wIndex: 0 (HID interface number)
//    - wLength: How many bytes we expect back
//
// 2. UPS responds with the requested report data
//
// 3. Our callback fires when data arrives
//
// THE CRITICAL FIX - WHY TWO EVENT HANDLERS:
// ────────────────────────────────────────────────────────────────────────────
// In the wait loop, we MUST call BOTH:
// - usb_host_lib_handle_events()    → Processes control transfer at hardware level
// - usb_host_client_handle_events() → Fires our callback when data arrives
//
// If we only call client events (like we did initially), control transfers
// never complete because the library-level processing doesn't happen!
//
// This took HOURS to debug because:
// - Interrupt transfers only need client events
// - Control transfers need BOTH lib and client events
// - The documentation doesn't clearly explain this difference
//
static esp_err_t get_hid_report(uint8_t report_id, uint8_t *buffer, size_t buffer_size, size_t *actual_length)
{
    if (ups_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Acquire mutex: Only one USB transfer at a time
    // This prevents interrupt and control transfers from interfering
    if (xSemaphoreTake(transfer_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire transfer mutex for GET_REPORT");
        return ESP_ERR_TIMEOUT;
    }

    // Prepare USB transfer for control request
    usb_transfer_t *transfer;
    esp_err_t err = usb_host_transfer_alloc(buffer_size + 8, 0, &transfer);  // +8 for setup packet
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate control transfer: %s", esp_err_to_name(err));
        xSemaphoreGive(transfer_mutex);
        return err;
    }

    // Setup control GET_REPORT request
    // Request Type: 0xA1 = Device-to-Host, Class, Interface
    // Request: 0x01 = GET_REPORT
    // Value: (ReportType << 8) | ReportID, where ReportType=3 for Feature Report
    // Index: Interface number (0)
    // Length: Expected report size
    // NOTE: Changed from Input Reports (type 1) to Feature Reports (type 3)
    // because voltage/load/frequency are synchronous polled values, not async events
    transfer->device_handle = ups_device;
    transfer->bEndpointAddress = 0x00;  // Control endpoint
    transfer->callback = transfer_callback;
    transfer->context = NULL;
    transfer->num_bytes = buffer_size + 8;
    transfer->timeout_ms = 3000;

    // Fill setup packet
    usb_setup_packet_t *setup = (usb_setup_packet_t *)transfer->data_buffer;
    setup->bmRequestType = 0xA1;  // Device-to-Host, Class, Interface
    setup->bRequest = 0x01;        // GET_REPORT
    setup->wValue = (3 << 8) | report_id;  // Feature Report (type 3), Report ID
    setup->wIndex = HID_INTERFACE;
    setup->wLength = buffer_size;

    // Create semaphore if not already created
    if (transfer_done == NULL) {
        transfer_done = xSemaphoreCreateBinary();
        if (transfer_done == NULL) {
            ESP_LOGE(TAG, "Failed to create transfer semaphore");
            usb_host_transfer_free(transfer);
            xSemaphoreGive(transfer_mutex);
            return ESP_ERR_NO_MEM;
        }
    }

    // Submit transfer
    err = usb_host_transfer_submit_control(usb_client, transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to submit GET_REPORT for 0x%02X: %s", report_id, esp_err_to_name(err));
        usb_host_transfer_free(transfer);
        xSemaphoreGive(transfer_mutex);
        return err;
    }

    ESP_LOGD(TAG, "🔍 Requesting report ID 0x%02X...", report_id);

    // Wait for transfer completion
    // CRITICAL: Must process BOTH lib and client events for control transfers
    const int max_wait_ms = 2000;
    const int poll_interval_ms = 10;
    int waited_ms = 0;
    bool transfer_complete = false;

    // ═══════════════════════════════════════════════════════════════════════
    // THE CRITICAL WAIT LOOP - This is what makes control transfers work!
    // ═══════════════════════════════════════════════════════════════════════
    while (waited_ms < max_wait_ms && !transfer_complete) {
        // STEP 1: Process library-level events (hardware USB processing)
        // This is ESSENTIAL for control transfers to actually execute
        uint32_t event_flags;
        usb_host_lib_handle_events(pdMS_TO_TICKS(5), &event_flags);

        // STEP 2: Process client-level events (fires our callback)
        // This checks if our transfer_callback has been called
        usb_host_client_handle_events(usb_client, pdMS_TO_TICKS(5));

        // STEP 3: Check if callback fired (semaphore was given)
        // Non-blocking check (timeout=0) so we keep looping
        if (xSemaphoreTake(transfer_done, 0) == pdTRUE) {
            transfer_complete = true;
            break;
        }
        waited_ms += poll_interval_ms;
    }
    // ═══════════════════════════════════════════════════════════════════════

    if (transfer_complete) {
        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
            // Data starts after 8-byte setup packet
            *actual_length = transfer->actual_num_bytes - 8;
            if (*actual_length > 0 && *actual_length <= buffer_size) {
                memcpy(buffer, transfer->data_buffer + 8, *actual_length);
                ESP_LOGI(TAG, "✅ GET_REPORT 0x%02X: %d bytes", report_id, *actual_length);
                err = ESP_OK;
            } else {
                err = ESP_ERR_INVALID_SIZE;
            }
        } else if (transfer->status == USB_TRANSFER_STATUS_STALL) {
            ESP_LOGI(TAG, "⚠️  Report 0x%02X not available (STALL)", report_id);
            err = ESP_ERR_NOT_SUPPORTED;
        } else {
            ESP_LOGI(TAG, "⚠️  GET_REPORT 0x%02X failed, status=%d", report_id, transfer->status);
            err = ESP_FAIL;
        }
        usb_host_transfer_free(transfer);
    } else {
        ESP_LOGW(TAG, "⚠️  GET_REPORT 0x%02X timeout after %dms, aborting", report_id, max_wait_ms);
        ESP_LOGW(TAG, "   Transfer status: %d (0=no_device, 1=completed, 2=error, 3=timed_out, 4=cancelled, 5=stall, 6=overflow, 7=skipped)",
                 transfer->status);

        // Cancel and free the transfer
        // Don't wait forever - the UPS doesn't support this report ID
        usb_host_transfer_free(transfer);
        err = ESP_ERR_NOT_SUPPORTED;
    }

    // Release mutex
    xSemaphoreGive(transfer_mutex);
    return err;
}

// Read HID report from interrupt endpoint
static esp_err_t read_hid_report(uint8_t *buffer, size_t buffer_size, size_t *actual_length)
{
    if (ups_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Acquire mutex to prevent concurrent transfers
    if (xSemaphoreTake(transfer_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire transfer mutex for interrupt read");
        return ESP_ERR_TIMEOUT;
    }

    // Create semaphore if not already created
    if (transfer_done == NULL) {
        transfer_done = xSemaphoreCreateBinary();
        if (transfer_done == NULL) {
            ESP_LOGE(TAG, "Failed to create transfer semaphore");
            xSemaphoreGive(transfer_mutex);
            return ESP_ERR_NO_MEM;
        }
    }

    // Prepare USB transfer
    usb_transfer_t *transfer;
    esp_err_t err = usb_host_transfer_alloc(buffer_size, 0, &transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate transfer: %s", esp_err_to_name(err));
        xSemaphoreGive(transfer_mutex);
        return err;
    }

    // Setup interrupt IN transfer
    transfer->device_handle = ups_device;
    transfer->bEndpointAddress = HID_INTERRUPT_IN_EP;
    transfer->callback = transfer_callback;
    transfer->context = NULL;
    transfer->num_bytes = buffer_size;
    transfer->timeout_ms = 300;  // Short timeout - UPS sends every ~8s

    // Submit transfer
    err = usb_host_transfer_submit(transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to submit transfer: %s", esp_err_to_name(err));
        usb_host_transfer_free(transfer);
        xSemaphoreGive(transfer_mutex);
        return err;
    }

    // Wait for transfer to complete while processing USB events
    // The callback can ONLY fire when usb_host_client_handle_events() is called
    // So we must poll events while waiting, not just block on semaphore
    ESP_LOGD(TAG, "⏳ Waiting for transfer completion (endpoint 0x%02X)...", HID_INTERRUPT_IN_EP);

    // Poll for up to 500ms (interrupt data comes every ~8s from UPS)
    // Shorter timeout means we return to the main loop faster for poll cycles
    const int max_wait_ms = 500;
    const int poll_interval_ms = 10;
    int waited_ms = 0;
    bool transfer_complete = false;

    while (waited_ms < max_wait_ms && !transfer_complete) {
        // Process USB client events - this is where callbacks fire!
        usb_host_client_handle_events(usb_client, pdMS_TO_TICKS(poll_interval_ms));

        // Check if semaphore was signaled (non-blocking check)
        if (xSemaphoreTake(transfer_done, 0) == pdTRUE) {
            transfer_complete = true;
            break;
        }

        waited_ms += poll_interval_ms;
    }

    if (transfer_complete) {
        ESP_LOGD(TAG, "🔔 Transfer callback fired, status=%d (after %dms)", transfer->status, waited_ms);

        // Copy data and get actual length
        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
            *actual_length = transfer->actual_num_bytes;
            memcpy(buffer, transfer->data_buffer, transfer->actual_num_bytes);

            if (*actual_length > 0) {
                ESP_LOGI(TAG, "✅ HID report received: %d bytes", *actual_length);
                ESP_LOG_BUFFER_HEX_LEVEL(TAG, buffer, (*actual_length < 16) ? *actual_length : 16, ESP_LOG_INFO);
            }
            err = ESP_OK;
        } else if (transfer->status == USB_TRANSFER_STATUS_TIMED_OUT) {
            ESP_LOGD(TAG, "⏱️  Transfer timed out (USB level) - device not sending data");
            err = ESP_ERR_TIMEOUT;
        } else if (transfer->status == USB_TRANSFER_STATUS_STALL) {
            ESP_LOGW(TAG, "⚠️  Transfer stalled - endpoint may not be ready");
            err = ESP_FAIL;
        } else if (transfer->status == USB_TRANSFER_STATUS_ERROR) {
            ESP_LOGW(TAG, "❌ Transfer error");
            err = ESP_FAIL;
        } else if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
            ESP_LOGW(TAG, "❌ Device disconnected");
            err = ESP_FAIL;
        } else {
            ESP_LOGW(TAG, "❌ Transfer failed with unknown status: %d", transfer->status);
            err = ESP_FAIL;
        }

        // CRITICAL: Only free after callback has fired
        usb_host_transfer_free(transfer);
    } else {
        // Timeout waiting for callback - release mutex so other transfers can proceed,
        // then keep processing events until callback fires to avoid memory corruption
        ESP_LOGW(TAG, "⚠️  App-level timeout (%dms), releasing mutex and waiting for late callback...", max_wait_ms);
        xSemaphoreGive(transfer_mutex);  // Release mutex BEFORE waiting

        // Keep processing events until callback fires (with a hard limit)
        int late_wait_ms = 0;
        const int LATE_WAIT_MAX_MS = 5000;  // Give up after 5s more
        while (!transfer_complete && late_wait_ms < LATE_WAIT_MAX_MS) {
            usb_host_client_handle_events(usb_client, pdMS_TO_TICKS(poll_interval_ms));
            if (xSemaphoreTake(transfer_done, 0) == pdTRUE) {
                transfer_complete = true;
                ESP_LOGW(TAG, "✅ Late callback received, freeing transfer (waited %dms more)", late_wait_ms);
                usb_host_transfer_free(transfer);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
            late_wait_ms += poll_interval_ms;
        }
        if (!transfer_complete) {
            ESP_LOGE(TAG, "❌ Late callback never fired after %dms, leaking transfer", late_wait_ms);
            // Can't free transfer without callback -- it's a small leak but prevents crash
        }
        return ESP_ERR_TIMEOUT;
    }

    // Release mutex
    xSemaphoreGive(transfer_mutex);
    return err;
}

esp_err_t usb_host_init(void)
{
    ESP_LOGI(TAG, "DEBUG: usb_host_init() called");
    ESP_LOGI(TAG, "🚀 Initializing USB Host for APC UPS");
    ESP_LOGW(TAG, "⚠️ Note: Many ESP32-S3 dev boards don't expose USB OTG pins");

    // Create mutex
    ESP_LOGI(TAG, "DEBUG: Creating USB mutex");
    usb_mutex = xSemaphoreCreateMutex();
    if (usb_mutex == NULL) {
        ESP_LOGE(TAG, "❌ Failed to create USB mutex");
        return ESP_FAIL;
    }

    // Create transfer mutex to serialize transfers
    transfer_mutex = xSemaphoreCreateMutex();
    if (transfer_mutex == NULL) {
        ESP_LOGE(TAG, "❌ Failed to create transfer mutex");
        return ESP_FAIL;
    }

    // Install USB Host library
    ESP_LOGI(TAG, "DEBUG: Installing USB Host library");
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    esp_err_t ret = usb_host_install(&host_config);
    ESP_LOGI(TAG, "DEBUG: usb_host_install returned: 0x%x", ret);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to install USB host: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "💡 Your board may not support USB OTG on external pins");
        ESP_LOGW(TAG, "📝 Continuing with simulated data...");
        return ret;
    }

    // Register USB host client
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = usb_host_client_event_cb,
            .callback_arg = NULL
        }
    };

    ret = usb_host_client_register(&client_config, &usb_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to register USB client: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "💡 USB OTG not available on this board");
        usb_host_uninstall();
        return ret;
    }

    ESP_LOGI(TAG, "✅ USB Host initialized successfully");
    ESP_LOGI(TAG, "🔍 Waiting for APC UPS (VID=%04X, PID=%04X or %04X)", APC_VID, APC_PID_BACKUPS, APC_PID_SMARTUPS);

    return ESP_OK;
}

void usb_host_task(void *arg)
{
    ESP_LOGI(TAG, "📡 USB Host task started");
    ESP_LOGI(TAG, "DEBUG: Polling for USB events every 100ms");

    uint8_t report_buffer[64];
    size_t report_len;
    int error_count = 0;
    const int MAX_ERRORS = 10;
    int loop_count = 0;
    int poll_cycle = 0;
    int64_t last_poll_time = 0;  // Time of last feature report poll cycle
    const int64_t POLL_INTERVAL_MS = 10000;  // Poll feature reports every 10 seconds
    bool initial_poll_done = false;

    // Report IDs to actively poll via GET_REPORT (feature reports)
    // Only include reports that actually respond on APC Back-UPS XS 1000M (051D:0003)
    // Reports that STALL on this UPS: 0x09, 0x15, 0x16, 0x17, 0x18, 0x20, 0x24,
    //   0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x50, 0x52
    // Note: 0x31 (input voltage) and 0x50 (load %) are NOT available as feature
    // reports on this UPS -- they may arrive as interrupt IN reports only.
    // Note: 0x09 returns garbage (191.12V), 0x0D is authoritative for voltage.
    const uint8_t poll_reports[] = {
        // === CRITICAL METRICS ===
        0x0A,  // Battery runtime (seconds, 16-bit LE) - WORKS
        0x06,  // Status flags (ACPresent, Charging, etc.) - WORKS
        0x0D,  // Battery voltage (16-bit LE / 100) - WORKS, authoritative

        // === BATTERY INFO ===
        0x08,  // Battery nominal voltage (per-cell, 0x78 = 1.2V PbAc)
        0x0E,  // Full charge capacity
        0x0F,  // Battery charge warning threshold (50%)
        0x11,  // Battery charge low threshold (10%)
        0x03,  // Battery chemistry type (1 = PbAc)
        0x07,  // UPS manufacture date
        0x10,  // Beeper status (enabled/disabled/muted)
    };
    const int num_poll_reports = sizeof(poll_reports) / sizeof(poll_reports[0]);

    while (1) {
        loop_count++;

        // 🔍 Evil Hardware Hacker: Check if we need to request the HID Report Descriptor
        // This must be done from the main task context, NOT the event callback
        if (ups_connected && !hid_report_desc_requested) {
            // We set the flag to true immediately to prevent multiple requests
            hid_report_desc_requested = true; 
            request_hid_report_descriptor(ups_device, HID_INTERFACE);
        }

        // Log every 50 loops (5 seconds) to show task is alive
        if (loop_count % 50 == 0) {
            ESP_LOGI(TAG, "DEBUG: USB task alive, loop %d, UPS connected: %d", loop_count, ups_connected);
        }

        // CRITICAL: Handle USB host LIBRARY events first (device connection/disconnection)
        uint32_t event_flags;
        esp_err_t err = usb_host_lib_handle_events(pdMS_TO_TICKS(10), &event_flags);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "⚠️ USB lib event error: %s", esp_err_to_name(err));
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGW(TAG, "⚠️ No USB clients registered");
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "DEBUG: All devices freed");
        }

        // Handle USB host CLIENT events (our callback)
        err = usb_host_client_handle_events(usb_client, pdMS_TO_TICKS(10));

        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            error_count++;
            ESP_LOGW(TAG, "⚠️ USB client event error (%d/%d): %s",
                     error_count, MAX_ERRORS, esp_err_to_name(err));

            if (error_count >= MAX_ERRORS) {
                ESP_LOGE(TAG, "❌ USB Host failed too many times, disabling USB host");
                ESP_LOGE(TAG, "💡 Hint: This board may not support USB OTG on external pins");
                ESP_LOGE(TAG, "📝 Using simulated UPS data only");
                vTaskDelete(NULL);  // Kill this task
                return;
            }

            vTaskDelay(pdMS_TO_TICKS(1000));  // Back off on errors
            continue;
        } else if (err == ESP_OK) {
            error_count = 0;  // Reset error count on success
            ESP_LOGI(TAG, "DEBUG: USB client event received (not timeout)");
        }

        // If UPS is connected, try to read HID reports
        if (ups_connected && ups_device != NULL) {
            // Passive: Read interrupt transfers (UPS sends automatically)
            err = read_hid_report(report_buffer, sizeof(report_buffer), &report_len);

            if (err == ESP_OK && report_len > 0) {
                // First byte is usually the report ID
                uint8_t report_id = report_buffer[0];

                ESP_LOGD(TAG, "📥 HID Report ID: 0x%02X, Length: %d", report_id, report_len);

                // Parse the report
                apc_hid_parse_report(report_id, report_buffer, report_len, NULL);
            }

            // Time-based feature report polling (loop_count is unreliable because
            // read_hid_report blocks for seconds at a time)
            int64_t now_ms = esp_timer_get_time() / 1000;
            bool time_for_poll = !initial_poll_done || 
                                 (now_ms - last_poll_time) >= POLL_INTERVAL_MS;
            
            if (time_for_poll) {
                initial_poll_done = true;
                last_poll_time = now_ms;
                ESP_LOGI(TAG, "🔄 Active polling cycle %d: Requesting %d reports... (elapsed %lld ms since last poll)", poll_cycle++, num_poll_reports, initial_poll_done ? (now_ms - last_poll_time + POLL_INTERVAL_MS) : 0);

                for (int i = 0; i < num_poll_reports; i++) {
                    uint8_t report_id = poll_reports[i];
                    // CRITICAL: Clear buffer before GET_REPORT to prevent stale data
                    memset(report_buffer, 0, sizeof(report_buffer));
                    err = get_hid_report(report_id, report_buffer, sizeof(report_buffer), &report_len);

                    if (err == ESP_OK && report_len > 0) {
                        // Log raw hex of feature report response
                        ESP_LOGI(TAG, "📥 GET_REPORT 0x%02X: %d bytes", report_id, report_len);
                        char hex_buf[128] = {0};
                        int pos = 0;
                        for (int b = 0; b < report_len && b < 16; b++) {
                            pos += snprintf(hex_buf + pos, sizeof(hex_buf) - pos, "%02X ", report_buffer[b]);
                        }
                        ESP_LOGI(TAG, "   Raw: %s", hex_buf);
                        // Parse the polled report
                        apc_hid_parse_report(report_id, report_buffer, report_len, NULL);
                    } else if (err != ESP_OK) {
                        ESP_LOGW(TAG, "⚠️ GET_REPORT 0x%02X failed: %s", report_id, esp_err_to_name(err));
                    }

                    // Small delay between polls to avoid overwhelming UPS
                    vTaskDelay(pdMS_TO_TICKS(20));
                }

                ESP_LOGI(TAG, "✅ Polling cycle %d complete", poll_cycle - 1);
            }
        }

        // Small delay
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool usb_ups_is_connected(void)
{
    return ups_connected;
}

