/*
 * Periodic DHT11 polling task and the shared reading it publishes.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;           /* a reading has succeeded at least once */
    bool stale;           /* no valid reading within the stale timeout */
    int16_t temperature_dc;  /* tenths of a degree Celsius */
    int16_t humidity_dpct;   /* tenths of a percent RH */
    uint32_t age_s;       /* seconds since the last valid reading */
    uint32_t reads_ok;
    uint32_t reads_failed;
    esp_err_t last_error; /* result of the most recent attempt */
} sensor_snapshot_t;

/*
 * Configure the sensor pin and start the polling task.
 */
esp_err_t sensor_start(void);

/*
 * Copy the current state. Safe to call from any task.
 */
void sensor_get(sensor_snapshot_t *out);

/*
 * Human-readable name for a value returned in last_error.
 */
const char *sensor_error_name(esp_err_t err);

/*
 * The fitted sensor's model name, for logs and the web UI.
 */
const char *sensor_model(void);

#ifdef __cplusplus
}
#endif
