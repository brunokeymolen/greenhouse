/*
 * Runtime configuration, persisted in NVS.
 *
 * Kconfig supplies the compile-time defaults; NVS overrides them at runtime so
 * the device can be configured from the web UI without reflashing.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bump this on ANY change to greenhouse_config_t, including during development.
 * Version 2 was briefly reused across three different layouts while fields were
 * being added, which the length check caught but only by luck: two layouts could
 * as easily have matched in size and been read as each other.
 */
#define GREENHOUSE_CONFIG_VERSION 3

/* Including the terminator. Kept small: it is shown on one line in the UI. */
#define GREENHOUSE_DEVICE_NAME_MAX 24

/* 32 bytes is the 802.11 SSID limit, plus a terminator. */
#define GREENHOUSE_AP_SSID_MAX 33

typedef enum {
    FAN_MODE_OFF = 0,   /* forced off, conditions ignored */
    FAN_MODE_AUTO = 1,  /* the control state machine decides */
    FAN_MODE_ON = 2,    /* forced on, conditions ignored */
} fan_mode_t;

typedef struct {
    uint32_t version;
    int16_t temp_threshold_c;
    uint8_t humidity_threshold_pct;
    uint16_t start_delay_s;
    uint16_t max_fan_duration_s;
    uint16_t grace_period_s;
    uint8_t sensor_poll_interval_s;
    bool relay_active_low;
    /* Added in version 2. Either input can be taken out of the fan decision
     * while still being measured, logged and charted. */
    bool temp_enabled;
    bool humidity_enabled;
    uint8_t fan_mode;  /* fan_mode_t */
    /* What the relay actually switches, for the UI: "Fan", "Cooling",
     * "Water pump". Purely cosmetic; nothing branches on it. */
    char device_name[GREENHOUSE_DEVICE_NAME_MAX];
    /* Access point name. Empty means derive Greenhouse-XXXX from the MAC, which
     * keeps several units distinguishable without configuring each one. */
    char ap_ssid[GREENHOUSE_AP_SSID_MAX];
} greenhouse_config_t;

/*
 * Load from NVS, falling back to defaults if absent, corrupt, or written by a
 * different config version. Always leaves a usable configuration in place.
 *
 * Requires nvs_flash_init() to have run first.
 */
esp_err_t config_load(void);

/*
 * Copy the current configuration. Safe from any task.
 */
void config_get(greenhouse_config_t *out);

/*
 * Validate and persist. Returns ESP_ERR_INVALID_ARG without writing anything if
 * a field is out of bounds; pass `err_field` to learn which one.
 */
esp_err_t config_save(const greenhouse_config_t *in, const char **err_field);

/*
 * Restore compiled-in defaults and persist them.
 */
esp_err_t config_reset(void);

/*
 * Fill `out` with the compiled-in defaults without touching stored state.
 */
void config_defaults(greenhouse_config_t *out);

#ifdef __cplusplus
}
#endif
