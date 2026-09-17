/*
 * Copyright (c) 2026 Bruno Keymolen
 * SPDX-License-Identifier: MIT
 */

/*
 * Greenhouse ESP8266 relay controller.
 *
 * Milestones 1-3: boots, polls the sensor, and serves a status page from its own
 * access point. The relay is not driven yet; see docs/DESIGN.md.
 *
 * Serial note: with CONFIG_GREENHOUSE_SENSOR_GPIO set to 2 the UART console stays
 * usable. If the sensor is moved to GPIO1/TX, serial logging and the sensor
 * share a pin and the web UI becomes the only usable console.
 */

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "controller.h"
#include "factory_reset.h"
#include "history.h"
#include "relay.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "sensor.h"
#include "wifi.h"
#include "web.h"

static const char *TAG = "greenhouse";

void app_main(void)
{
    ESP_LOGI(TAG, "greenhouse controller starting, sdk %s", esp_get_idf_version());

    /* The Wi-Fi driver keeps calibration data in NVS. A partition left over from
     * an older layout is recoverable by erasing it rather than failing to boot. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_LOGW(TAG, "nvs has no free pages, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Config must load before the sensor task reads its poll interval. */
    err = config_load();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config_load failed: %s, continuing on defaults",
                 esp_err_to_name(err));
    }

    /*
     * Relay first, before Wi-Fi and the web server. GPIO0 is the boot strap and
     * sits high until we drive it, so on an active-high carrier the relay is
     * energised until this call. Every millisecond before it is fan-on time.
     */
    err = relay_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "relay_init failed: %s", esp_err_to_name(err));
    }

    /*
     * After config_load(), which creates the configuration this may overwrite,
     * and after relay_init(), so the relay is already in its off state before
     * any confirmation clicks. Before Wi-Fi, so a reset takes effect on the
     * network the device brings up rather than the next boot's.
     */
    factory_reset_check();

    err = history_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "history_init failed: %s", esp_err_to_name(err));
    }

    /* Start the sensor first so the UI has something to show immediately. */
    err = sensor_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sensor_start failed: %s", esp_err_to_name(err));
    }

#if CONFIG_GREENHOUSE_DIAG_NO_WIFI
    ESP_LOGW(TAG, "diagnostic build: Wi-Fi and web server disabled");
#else
    ESP_ERROR_CHECK(wifi_start());
    ESP_ERROR_CHECK(web_start());
#endif

    err = controller_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller_start failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "ready, free heap %u bytes", esp_get_free_heap_size());

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "free heap %u bytes", esp_get_free_heap_size());
    }
}
