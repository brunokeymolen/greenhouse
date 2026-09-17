/*
 * Copyright (c) 2026 Bruno Keymolen
 * SPDX-License-Identifier: MIT
 */

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
#define GREENHOUSE_CONFIG_VERSION 5

/* Including the terminator. Kept small: it is shown on one line in the UI. */
#define GREENHOUSE_DEVICE_NAME_MAX 24

/* 32 bytes is the 802.11 SSID limit, plus a terminator. */
#define GREENHOUSE_SSID_MAX 33

/* A WPA2 passphrase is 8 to 63 printable ASCII characters, plus a terminator. */
#define GREENHOUSE_PASSWORD_MAX 64
#define GREENHOUSE_PASSWORD_MIN 8

typedef enum {
    /* Run our own access point. The only mode that needs no other equipment,
     * so it is what a factory reset returns to. */
    GH_WIFI_MODE_AP = 0,
    /* Join an existing network. If the join fails at boot the device falls back
     * to a recovery access point for that boot only; see wifi.h. What is stored
     * here never changes by itself, so the next boot tries the network again. */
    GH_WIFI_MODE_STA = 1,
} greenhouse_wifi_mode_t;

typedef enum {
    /* Switch on when a reading is at or above its threshold: ventilating a
     * greenhouse that is too hot or too damp. */
    GH_TRIGGER_ABOVE = 0,
    /* Switch on when a reading is at or below its threshold: a lamp or a
     * heater holding a winter minimum. */
    GH_TRIGGER_BELOW = 1,
} greenhouse_trigger_dir_t;

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
    char ap_ssid[GREENHOUSE_SSID_MAX];
    /* Added in version 4: the Wi-Fi settings, previously compile-time only. */
    uint8_t wifi_mode;  /* greenhouse_wifi_mode_t */
    /* Our own AP's passphrase. Empty means an open network, which is a
     * deliberate choice for bring-up, never something the UI can do by
     * accident: an empty field there means "leave unchanged". */
    char ap_password[GREENHOUSE_PASSWORD_MAX];
    /* The network to join in GH_WIFI_MODE_STA. */
    char sta_ssid[GREENHOUSE_SSID_MAX];
    char sta_password[GREENHOUSE_PASSWORD_MAX];
    /* Added in version 5. Which side of the thresholds switches the relay on.
     * It applies to both inputs at once: one relay serves one purpose, and
     * mixing directions across inputs would mean a load asked to run both when
     * it is too cold and when it is too damp. */
    uint8_t trigger_direction;  /* greenhouse_trigger_dir_t */
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
 * Restore compiled-in defaults and persist them. This is the factory reset: it
 * returns the device to its own access point with the built-in password, which
 * is the only state reachable with no prior knowledge of the installation.
 */
esp_err_t config_reset(void);

/*
 * Fill `out` with the compiled-in defaults without touching stored state.
 */
void config_defaults(greenhouse_config_t *out);

#ifdef __cplusplus
}
#endif
