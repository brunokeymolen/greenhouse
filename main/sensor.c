/*
 * Copyright (c) 2026 Bruno Keymolen
 * SPDX-License-Identifier: MIT
 */

#include "sensor.h"

#include <string.h>

#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "dht.h"
#include "history.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/*
 * A single bad frame is normal on a DHT11, so retry a few times inside one poll
 * before recording a failure.
 */
#define SENSOR_READ_ATTEMPTS 3

static const char *TAG = "sensor";

static SemaphoreHandle_t s_lock;

static struct {
    bool valid;
    int16_t temperature_dc;
    int16_t humidity_dpct;
    int64_t last_ok_us;
    uint32_t reads_ok;
    uint32_t reads_failed;
    esp_err_t last_error;
} s_state;

const char *sensor_model(void)
{
    return dht_model();
}

const char *sensor_error_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return "ok";
    case ESP_ERR_TIMEOUT:
        return "timeout";
    case ESP_ERR_INVALID_CRC:
        return "checksum";
    case ESP_ERR_INVALID_RESPONSE:
        return "empty frame";
    case ESP_ERR_INVALID_STATE:
        return "not initialised";
    default:
        return "error";
    }
}

void sensor_get(sensor_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));

    if (s_lock == NULL) {
        out->last_error = ESP_ERR_INVALID_STATE;
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    out->valid = s_state.valid;
    out->temperature_dc = s_state.temperature_dc;
    out->humidity_dpct = s_state.humidity_dpct;
    out->reads_ok = s_state.reads_ok;
    out->reads_failed = s_state.reads_failed;
    out->last_error = s_state.last_error;

    if (s_state.valid) {
        int64_t age_us = esp_timer_get_time() - s_state.last_ok_us;
        out->age_s = (uint32_t)(age_us / 1000000);
        out->stale = out->age_s >= CONFIG_GREENHOUSE_SENSOR_STALE_TIMEOUT_S;
    } else {
        out->stale = true;
    }

    xSemaphoreGive(s_lock);
}

/*
 * Dump the raw frame and the pulse width of every bit. A decode problem shows up
 * as pulse widths clustered near the 45 us threshold; a genuinely noisy sensor
 * shows clean widths with changing data.
 */
static void sensor_log_frame(void)
{
    uint8_t bytes[5];
    int pulse_us[40];
    dht_last_frame(bytes, pulse_us);

    ESP_LOGI(TAG, "raw %02x %02x %02x %02x %02x (sum %02x)",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4],
             (uint8_t)(bytes[0] + bytes[1] + bytes[2] + bytes[3]));

    char line[8 * 5 + 1];
    for (int b = 0; b < 5; b++) {
        int n = 0;
        for (int i = 0; i < 8; i++) {
            n += snprintf(line + n, sizeof(line) - n, "%d ", pulse_us[b * 8 + i]);
        }
        ESP_LOGI(TAG, "  byte%d us: %s", b, line);
    }
}

static void sensor_task(void *arg)
{
    /* The DHT11 needs about a second after power-up before its first
     * conversion is meaningful. */
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {
        /* Re-read each cycle so a change from the web UI applies without a
         * reboot, from the next reading onwards. */
        greenhouse_config_t cfg;
        config_get(&cfg);
        const TickType_t period = pdMS_TO_TICKS(cfg.sensor_poll_interval_s * 1000);

        /* Zeroed because it is passed to history_add() even on failure,
         * where the values are ignored but must still be initialised. */
        dht_reading_t reading = {0};
        esp_err_t err = ESP_FAIL;

        for (int attempt = 0; attempt < SENSOR_READ_ATTEMPTS; attempt++) {
            err = dht_read(&reading);
            if (err == ESP_OK) {
                break;
            }
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);

        s_state.last_error = err;

        if (err == ESP_OK) {
            s_state.valid = true;
            s_state.temperature_dc = reading.temperature_dc;
            s_state.humidity_dpct = reading.humidity_dpct;
            s_state.last_ok_us = esp_timer_get_time();
            s_state.reads_ok++;
        } else {
            s_state.reads_failed++;
        }

        xSemaphoreGive(s_lock);

        history_add(err == ESP_OK, reading.temperature_dc, reading.humidity_dpct);

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "%d.%d C, %d.%d %%RH",
                     reading.temperature_dc / 10, abs(reading.temperature_dc % 10),
                     reading.humidity_dpct / 10, abs(reading.humidity_dpct % 10));

            /* Outside the part's rated range the value is reported but should
             * not be trusted. A greenhouse really can sit outside it. */
            if (reading.humidity_dpct < dht_humidity_min_dpct() ||
                reading.humidity_dpct > dht_humidity_max_dpct()) {
                ESP_LOGW(TAG, "humidity %d.%d %%RH is outside the %s's rated "
                              "%d-%d %%RH range, treat as unreliable",
                         reading.humidity_dpct / 10, abs(reading.humidity_dpct % 10),
                         dht_model(),
                         dht_humidity_min_dpct() / 10, dht_humidity_max_dpct() / 10);
            }
            if (reading.temperature_dc < dht_temperature_min_dc() ||
                reading.temperature_dc > dht_temperature_max_dc()) {
                ESP_LOGW(TAG, "temperature %d.%d C is outside the %s's rated "
                              "%d-%d C range, treat as unreliable",
                         reading.temperature_dc / 10, abs(reading.temperature_dc % 10),
                         dht_model(),
                         dht_temperature_min_dc() / 10, dht_temperature_max_dc() / 10);
            }

            sensor_log_frame();
        } else {
            ESP_LOGW(TAG, "read failed after %d attempts: %s",
                     SENSOR_READ_ATTEMPTS, sensor_error_name(err));
        }

        vTaskDelay(period);
    }
}

esp_err_t sensor_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = dht_init(CONFIG_GREENHOUSE_SENSOR_GPIO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dht_init on GPIO%d failed: %s",
                 CONFIG_GREENHOUSE_SENSOR_GPIO, esp_err_to_name(err));
        return err;
    }

    s_state.last_error = ESP_ERR_INVALID_STATE;

    if (xTaskCreate(sensor_task, "sensor", 2048, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    greenhouse_config_t cfg;
    config_get(&cfg);
    ESP_LOGI(TAG, "polling %s on GPIO%d every %d s",
             dht_model(), CONFIG_GREENHOUSE_SENSOR_GPIO, cfg.sensor_poll_interval_s);

    return ESP_OK;
}
