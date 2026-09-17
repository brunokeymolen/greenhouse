/*
 * Copyright (c) 2026 Bruno Keymolen
 * SPDX-License-Identifier: MIT
 */

/*
 * Wi-Fi bring-up: our own access point, or joining an existing network.
 *
 * Which one is stored in the configuration and never changes by itself. If
 * joining fails at boot the device raises a *recovery* access point instead,
 * but only for that boot: nothing is written, so the next restart tries the
 * configured network again. That way a router that was merely slow to come
 * back, or briefly renamed, does not permanently demote the device to AP mode
 * behind the owner's back.
 *
 * Recovery mode is not a way back in for someone who has forgotten their
 * password. It uses the configured AP password, precisely so that knocking the
 * house router offline does not hand an attacker a network with a documented
 * default password and a relay on the other end. Forgetting the password is
 * what the reset-button factory reset is for; see factory_reset.h.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* Serving our own access point, as configured. */
    GH_WIFI_STATE_AP = 0,
    /* Joined the configured network. */
    GH_WIFI_STATE_STA = 1,
    /* Station mode was configured but could not associate, so the access point
     * is up for this boot only. */
    GH_WIFI_STATE_RECOVERY = 2,
} gh_wifi_state_t;

/*
 * Bring up the radio in whichever mode the configuration selects.
 *
 * Requires nvs_flash_init() and config_load() to have run first; the Wi-Fi
 * driver stores calibration data in NVS and the mode comes from the config.
 *
 * Returns once the interface is started. In station mode that is before the
 * network has been joined: association is reported asynchronously, and the web
 * server is usable on the access point either way.
 */
esp_err_t wifi_start(void);

/*
 * The network name in use: ours when serving an access point, the one we joined
 * in station mode. Empty before wifi_start() runs.
 */
const char *wifi_ssid(void);

/*
 * The address the web UI is reachable on, as a dotted string. 192.168.4.1 for
 * our own access point; whatever DHCP handed out in station mode, or "0.0.0.0"
 * before a lease arrives.
 */
const char *wifi_ip(void);

/*
 * Number of stations currently associated with our access point. Always 0 in
 * station mode, where we are the client.
 */
int wifi_client_count(void);

gh_wifi_state_t gh_wifi_state(void);

/* Enough to choose from without spending heap on a long tail of weak signals. */
#define GREENHOUSE_SCAN_MAX 16

typedef struct {
    char ssid[GREENHOUSE_SSID_MAX];
    int8_t rssi;   /* dBm, closer to zero is stronger */
    bool secure;   /* anything other than an open network */
} wifi_scan_result_t;

/*
 * Scan for nearby networks, strongest first, at most `max` of them.
 *
 * Blocks for a second or two while the radio sweeps the channels, which
 * interrupts whatever the radio is otherwise doing: clients on our own access
 * point see the link stall until it finishes, and a station connection drops
 * briefly. Call it only when a user has actually asked to scan.
 *
 * Duplicate names are collapsed to their strongest sighting, and a network
 * whose name is empty or cannot be represented as text is left out; such a
 * network can still be joined by typing its name.
 */
esp_err_t wifi_scan(wifi_scan_result_t *out, size_t max, size_t *found);

/*
 * Stable identifier for the state, for JSON and logs: "ap", "sta", "recovery".
 */
const char *wifi_state_name(void);

#ifdef __cplusplus
}
#endif
