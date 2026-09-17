/*
 * Copyright (c) 2026 Bruno Keymolen
 * SPDX-License-Identifier: MIT
 */

/*
 * HTTP status UI served from the SoftAP.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start the web server. Call after wifi_ap_start().
 *
 * GET /            single-page status UI
 * GET /api/status  JSON reading, sensor health and device stats
 */
esp_err_t web_start(void);

#ifdef __cplusplus
}
#endif
