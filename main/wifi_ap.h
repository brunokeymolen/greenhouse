/*
 * SoftAP so the controller can be reached without an existing network.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring up the access point. The SSID is Greenhouse-XXXX, where XXXX is the last
 * two bytes of the AP MAC address, so several units can coexist.
 *
 * Requires nvs_flash_init() to have run first; the Wi-Fi driver stores
 * calibration data there.
 */
esp_err_t wifi_ap_start(void);

/*
 * The SSID chosen by wifi_ap_start(), or an empty string before it runs.
 */
const char *wifi_ap_ssid(void);

/*
 * Number of stations currently associated.
 */
int wifi_ap_client_count(void);

#ifdef __cplusplus
}
#endif
