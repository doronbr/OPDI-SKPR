#pragma once
#include "esp_http_server.h"
#ifdef __cplusplus
extern "C" { 
#endif

void opdi_api_ws_register(httpd_handle_t server);

// Broadcast helpers (JSON strings)
void opdi_api_ws_broadcast(const char *json, size_t len);

#ifdef __cplusplus
}
#endif
