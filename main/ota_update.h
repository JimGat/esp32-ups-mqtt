#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include "esp_http_server.h"

/* Register all OTA and system management endpoints */
void register_ota_handlers(httpd_handle_t server);

#endif /* OTA_UPDATE_H */
