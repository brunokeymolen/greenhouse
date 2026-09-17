/*
 * Copyright (c) 2026 Bruno Keymolen
 * SPDX-License-Identifier: MIT
 */

#include "controller.h"

#include <string.h>

#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "relay.h"
#include "sensor.h"

/*
 * How often the state machine re-evaluates. Independent of the sensor poll
 * interval so timers stay accurate even if polling is slow.
 */
#define TICK_MS 1000

/*
 * No usable reading for this long and the fan is forced off. Isolated read
 * failures are ignored; this is about a sensor that has genuinely stopped
 * reporting, where continuing to run on a stale value is the unsafe choice.
 */
#define SENSOR_FAULT_AFTER_S 60

static const char *TAG = "controller";

static SemaphoreHandle_t s_lock;
static controller_status_t s_status;

/* Seconds elapsed in the current state, owned by the controller task. */
static uint32_t s_elapsed;

const char *controller_state_name(controller_state_t s)
{
    switch (s) {
    case CTRL_IDLE:         return "idle";
    case CTRL_WAITING:      return "waiting";
    case CTRL_FAN_ON:       return "fan on";
    case CTRL_GRACE:        return "grace";
    case CTRL_GRACE_FORCED: return "cooldown";
    case CTRL_FAULT:        return "sensor fault";
    case CTRL_MANUAL_OFF:   return "forced off";
    case CTRL_MANUAL_ON:    return "forced on";
    default:                return "unknown";
    }
}

void controller_get(controller_status_t *out)
{
    if (out == NULL) {
        return;
    }

    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}

static void enter(controller_state_t next)
{
    if (s_status.state == next) {
        return;
    }

    ESP_LOGI(TAG, "%s -> %s", controller_state_name(s_status.state),
             controller_state_name(next));

    s_status.state = next;
    s_elapsed = 0;
}

static void controller_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));

        greenhouse_config_t cfg;
        config_get(&cfg);

        sensor_snapshot_t sensor;
        sensor_get(&sensor);

        bool usable = sensor.valid && sensor.age_s < SENSOR_FAULT_AFTER_S;

        /*
         * Which side of the threshold switches the load on. Above is
         * ventilation: too hot or too damp. Below is a lamp or a heater holding
         * a minimum. The boundary belongs to both, so a reading exactly at the
         * threshold counts as triggered either way.
         *
         * Thresholds are configured in whole degrees and percent; readings
         * arrive in tenths, so scale the threshold rather than rounding the
         * reading and losing the DHT22's resolution at the boundary.
         *
         * A disabled input is still measured, logged and charted; it just does
         * not vote. With both disabled the fan never starts on its own, which is
         * allowed but surfaced in the status so it cannot be mistaken for a
         * fault.
         */
        bool below = cfg.trigger_direction == GH_TRIGGER_BELOW;
        int temp_thr = (int)cfg.temp_threshold_c * 10;
        int hum_thr = (int)cfg.humidity_threshold_pct * 10;

        bool temp_trig = usable && cfg.temp_enabled &&
                         (below ? sensor.temperature_dc <= temp_thr
                                : sensor.temperature_dc >= temp_thr);
        bool hum_trig = usable && cfg.humidity_enabled &&
                        (below ? sensor.humidity_dpct <= hum_thr
                               : sensor.humidity_dpct >= hum_thr);

        /*
         * Either input alone is enough. Ventilating, hot but dry still needs
         * air and so does cool but damp; heating or lighting, the same argument
         * runs the other way.
         */
        bool trig = temp_trig || hum_trig;

        xSemaphoreTake(s_lock, portMAX_DELAY);

        s_elapsed++;
        s_status.temp_trig = temp_trig;
        s_status.humidity_trig = hum_trig;
        s_status.temp_enabled = cfg.temp_enabled;
        s_status.humidity_enabled = cfg.humidity_enabled;
        s_status.auto_disabled = cfg.fan_mode == FAN_MODE_AUTO &&
                                 !cfg.temp_enabled && !cfg.humidity_enabled;
        s_status.mode = cfg.fan_mode;
        s_status.trigger_direction = cfg.trigger_direction;

        /*
         * A forced mode bypasses the state machine entirely, including the
         * sensor-fault override: in manual the reading is not part of the
         * decision, so losing it is not a reason to change the fan. Timers do
         * not advance while forced; returning to auto restarts them.
         */
        if (cfg.fan_mode != FAN_MODE_AUTO) {
            bool forced_on = (cfg.fan_mode == FAN_MODE_ON);
            enter(forced_on ? CTRL_MANUAL_ON : CTRL_MANUAL_OFF);

            s_status.fan_on = forced_on;
            s_status.in_state_s = s_elapsed;
            s_status.remaining_s = 0;

            xSemaphoreGive(s_lock);
            relay_set(forced_on);
            continue;
        }

        /* Back in auto after a forced mode: start the machine over rather than
         * resuming timers that were frozen. */
        if (s_status.state == CTRL_MANUAL_ON || s_status.state == CTRL_MANUAL_OFF) {
            enter(usable ? CTRL_IDLE : CTRL_FAULT);
        }

        /*
         * Sensor fault pre-empts every other state. Running a fan on a reading
         * we no longer trust is worse than not ventilating: a stuck sensor is
         * exactly the case max_fan_duration exists to bound, and here we know
         * the reading is gone rather than merely suspicious.
         */
        if (!usable) {
            enter(CTRL_FAULT);
        }

        uint32_t remaining = 0;

        switch (s_status.state) {
        case CTRL_FAULT:
            if (usable) {
                enter(CTRL_IDLE);
            }
            break;

        case CTRL_IDLE:
            if (trig) {
                enter(CTRL_WAITING);
            }
            break;

        case CTRL_WAITING:
            if (!trig) {
                /* Not sustained: back to idle without starting. */
                enter(CTRL_IDLE);
            } else if (s_elapsed >= cfg.start_delay_s) {
                enter(CTRL_FAN_ON);
            } else {
                remaining = cfg.start_delay_s - s_elapsed;
            }
            break;

        case CTRL_FAN_ON:
            if (s_elapsed >= cfg.max_fan_duration_s) {
                /* Bounded run: protects the fan and relay from a stuck sensor
                 * or a permanently open door. Unlike GRACE, the fan goes off. */
                ESP_LOGW(TAG, "max fan duration %us reached, forcing cooldown",
                         cfg.max_fan_duration_s);
                enter(CTRL_GRACE_FORCED);
            } else if (!trig) {
                enter(CTRL_GRACE);
            } else {
                remaining = cfg.max_fan_duration_s - s_elapsed;
            }
            break;

        case CTRL_GRACE:
            /* Fan keeps running: lets the air mix and stops short-cycling. */
            if (trig) {
                enter(CTRL_FAN_ON);
            } else if (s_elapsed >= cfg.grace_period_s) {
                enter(CTRL_IDLE);
            } else {
                remaining = cfg.grace_period_s - s_elapsed;
            }
            break;

        case CTRL_GRACE_FORCED:
            /* Locked out even if the condition still holds. */
            if (s_elapsed >= cfg.grace_period_s) {
                enter(trig ? CTRL_WAITING : CTRL_IDLE);
            } else {
                remaining = cfg.grace_period_s - s_elapsed;
            }
            break;

        case CTRL_MANUAL_OFF:
        case CTRL_MANUAL_ON:
            /* Handled above; unreachable here. */
            break;
        }

        bool want_fan = (s_status.state == CTRL_FAN_ON || s_status.state == CTRL_GRACE);

        s_status.fan_on = want_fan;
        s_status.in_state_s = s_elapsed;
        s_status.remaining_s = remaining;

        xSemaphoreGive(s_lock);

        /* Driven every tick, not only on change: idempotent, and it re-asserts
         * the correct level after a polarity change or any stray write. */
        relay_set(want_fan);
    }
}

esp_err_t controller_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Start in fault rather than idle: no reading has been taken yet, and idle
     * would imply we know conditions are normal. */
    s_status.state = CTRL_FAULT;

    if (xTaskCreate(controller_task, "controller", 2560, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "started");
    return ESP_OK;
}
