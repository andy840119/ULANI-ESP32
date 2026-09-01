#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Serves the embedded UI plus the JSON endpoints under /api on port 80. */
esp_err_t web_server_start(void);
void      web_server_stop(void);

#ifdef __cplusplus
}
#endif
