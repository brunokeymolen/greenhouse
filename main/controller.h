/*
 * Fan control state machine.
 *
 * Decides relay state from sensor readings and the timing rules in
 * docs/DESIGN.md. Sustained conditions are required before acting, so a single
 * bad reading cannot start or stop the fan.
 *
 * "Triggered" means a reading is past its threshold on the side the
 * configuration selects: at or above it for ventilation, at or below it for a
 * lamp or heater holding a winter minimum. Everything downstream of that test
 * -- the delays, the bounded run, the grace period -- is the same either way.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CTRL_IDLE = 0,      /* conditions normal, fan off */
    CTRL_WAITING,       /* past threshold, waiting out start_delay */
    CTRL_FAN_ON,        /* fan running */
    CTRL_GRACE,         /* conditions normal again, fan still running */
    CTRL_GRACE_FORCED,  /* max duration hit, fan off and locked out */
    CTRL_FAULT,         /* no usable reading, fan off */
    CTRL_MANUAL_OFF,    /* forced off by the mode switch */
    CTRL_MANUAL_ON,     /* forced on by the mode switch */
} controller_state_t;

typedef struct {
    controller_state_t state;
    bool fan_on;
    uint32_t in_state_s;   /* seconds in the current state */
    uint32_t remaining_s;  /* seconds until the current timer expires, 0 if none */
    bool temp_trig;         /* temperature is past its threshold */
    bool humidity_trig;     /* humidity is past its threshold */
    bool temp_enabled;      /* temperature votes in the fan decision */
    bool humidity_enabled;  /* humidity votes in the fan decision */
    bool auto_disabled;     /* neither input votes; the fan cannot start */
    uint8_t mode;           /* fan_mode_t currently in force */
    uint8_t trigger_direction;  /* greenhouse_trigger_dir_t in force */
} controller_status_t;

esp_err_t controller_start(void);

void controller_get(controller_status_t *out);

const char *controller_state_name(controller_state_t s);

#ifdef __cplusplus
}
#endif
