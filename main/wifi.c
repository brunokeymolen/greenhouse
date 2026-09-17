#include "wifi.h"

#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "tcpip_adapter.h"

static const char *TAG = "wifi";

static char s_ssid[GREENHOUSE_SSID_MAX];
static char s_ip[16] = "0.0.0.0";
static int s_clients;
static gh_wifi_state_t s_state = GH_WIFI_STATE_AP;

/* Set once the station has associated, which is what separates "this network is
 * wrong" from "this network went away for a moment". */
static bool s_sta_was_up;
static bool s_sta_is_up;
static int s_sta_attempts;

/*
 * Deadlines in microseconds on the esp_timer clock, or 0 for "not armed". They
 * are evaluated by the supervisor task below rather than by timer callbacks:
 * the work they trigger is a radio restart, which must not run inside the Wi-Fi
 * event callback that asks for it, and would stall every other esp_timer user
 * for its duration if it ran in the shared timer task.
 */
static int64_t s_idle_deadline;   /* recovery AP with nobody connected */
static int64_t s_down_deadline;   /* station associated once, gone since */
static bool s_want_recovery;      /* the station gave up; raise the AP */

static TaskHandle_t s_supervisor;

static esp_err_t start_ap(bool recovery);

static void supervisor_wake(void)
{
    if (s_supervisor != NULL) {
        xTaskNotifyGive(s_supervisor);
    }
}

/*
 * Our access point's name: whatever is configured, or Greenhouse-XXXX derived
 * from the radio's own MAC so two units on the same bench never collide.
 */
static esp_err_t ap_name(const greenhouse_config_t *cfg, char *out, size_t len)
{
    if (cfg->ap_ssid[0] != '\0') {
        strlcpy(out, cfg->ap_ssid, len);
        return ESP_OK;
    }

    uint8_t mac[6] = {0};
    esp_err_t err = esp_wifi_get_mac(ESP_IF_WIFI_AP, mac);
    if (err != ESP_OK) {
        return err;
    }

    snprintf(out, len, "Greenhouse-%02X%02X", mac[4], mac[5]);
    return ESP_OK;
}

static void note_ip(tcpip_adapter_if_t iface)
{
    tcpip_adapter_ip_info_t info;
    if (tcpip_adapter_get_ip_info(iface, &info) == ESP_OK) {
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&info.ip));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
            s_sta_was_up = true;
            s_sta_is_up = true;
            s_sta_attempts = 0;
            s_down_deadline = 0;
            s_state = GH_WIFI_STATE_STA;
            ESP_LOGI(TAG, "joined %s, browse to %s", s_ssid, s_ip);
        }
        return;
    }

    switch (event_id) {
    case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        s_sta_is_up = false;

        if (s_sta_was_up) {
            /*
             * The network exists and our credentials are good; it has just gone
             * away. Keep reconnecting rather than dropping to the recovery AP,
             * so a router reboot does not cost us the connection for good. The
             * deadline below is the backstop for a router that never returns.
             */
            if (s_down_deadline == 0) {
                s_down_deadline = esp_timer_get_time() +
                                  (int64_t)CONFIG_GREENHOUSE_STA_DOWN_RESTART_S * 1000000;
                ESP_LOGW(TAG, "lost %s (reason %d), reconnecting", s_ssid, event->reason);
            }
            esp_wifi_connect();
            break;
        }

        if (++s_sta_attempts < CONFIG_GREENHOUSE_STA_CONNECT_ATTEMPTS) {
            ESP_LOGW(TAG, "join of %s failed (reason %d), attempt %d of %d",
                     s_ssid, event->reason, s_sta_attempts,
                     CONFIG_GREENHOUSE_STA_CONNECT_ATTEMPTS);
            esp_wifi_connect();
            break;
        }

        ESP_LOGE(TAG, "could not join %s after %d attempts (reason %d)",
                 s_ssid, s_sta_attempts, event->reason);
        s_want_recovery = true;
        supervisor_wake();
        break;
    }

    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        s_clients++;
        /* Somebody is here to fix the configuration: stop counting down to a
         * restart that would drop them mid-form. */
        s_idle_deadline = 0;
        ESP_LOGI(TAG, "station " MACSTR " joined, AID=%d, %d connected",
                 MAC2STR(event->mac), event->aid, s_clients);
        break;
    }

#if CONFIG_GREENHOUSE_DIAG_WIFI_PROBES
    case WIFI_EVENT_AP_PROBEREQRECVED: {
        /* Every nearby device probes, so rate limit rather than flooding the
         * console and disturbing the sensor timing. */
        static int64_t last_us;
        int64_t now = esp_timer_get_time();
        if (now - last_us > 500000) {
            last_us = now;
            wifi_event_ap_probe_req_rx_t *event = (wifi_event_ap_probe_req_rx_t *)event_data;
            ESP_LOGI(TAG, "probe from " MACSTR ", rssi %d", MAC2STR(event->mac), event->rssi);
        }
        break;
    }
#endif

    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        if (s_clients > 0) {
            s_clients--;
        }
        ESP_LOGI(TAG, "station " MACSTR " left, AID=%d, %d connected",
                 MAC2STR(event->mac), event->aid, s_clients);
        if (s_state == GH_WIFI_STATE_RECOVERY && s_clients == 0) {
            s_idle_deadline = esp_timer_get_time() +
                              (int64_t)CONFIG_GREENHOUSE_RECOVERY_AP_IDLE_S * 1000000;
            supervisor_wake();
        }
        break;
    }

    default:
        break;
    }
}

/*
 * Runs the deferred work for the event handler above, and owns the two
 * deadlines. A plain one-second tick is enough: both timeouts are minutes long,
 * and polling keeps all the restart decisions in one readable place.
 */
static void supervisor_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

        if (s_want_recovery) {
            s_want_recovery = false;
            ESP_LOGW(TAG, "starting recovery access point for this boot only");
            esp_err_t err = start_ap(true);
            if (err != ESP_OK) {
                /* Without a radio there is no way to reach the device at all,
                 * so a clean restart beats sitting here dark. */
                ESP_LOGE(TAG, "recovery AP failed: %s, restarting",
                         esp_err_to_name(err));
                esp_restart();
            }
            continue;
        }

        int64_t now = esp_timer_get_time();

        if (s_idle_deadline != 0 && now >= s_idle_deadline) {
            /*
             * Nobody came to reconfigure us. The network we were told to join
             * may well be back by now, so restart and try it again rather than
             * sitting on an access point nobody is looking at.
             */
            ESP_LOGW(TAG, "recovery access point idle for %ds, restarting to retry %s",
                     CONFIG_GREENHOUSE_RECOVERY_AP_IDLE_S, "the configured network");
            esp_restart();
        }

        if (s_down_deadline != 0 && now >= s_down_deadline) {
            ESP_LOGW(TAG, "station down for %ds, restarting",
                     CONFIG_GREENHOUSE_STA_DOWN_RESTART_S);
            esp_restart();
        }
    }
}

static esp_err_t start_ap(bool recovery)
{
    greenhouse_config_t cfg;
    config_get(&cfg);

    /* Coming from a failed station attempt the radio is already running in a
     * different mode; it has to be stopped before it can be reconfigured. */
    if (recovery) {
        esp_wifi_disconnect();
        esp_err_t err = esp_wifi_stop();
        if (err != ESP_OK) {
            return err;
        }
    }

    esp_err_t err = ap_name(&cfg, s_ssid, sizeof(s_ssid));
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t wifi_cfg = {0};
    /*
     * The SSID field is 32 bytes and is not NUL-terminated; ssid_len says how
     * much of it counts. strlcpy would cap a full 32-character name at 31 and
     * leave a NUL inside the broadcast name, so copy by length instead.
     */
    size_t ssid_len = strnlen(s_ssid, sizeof(wifi_cfg.ap.ssid));
    memcpy(wifi_cfg.ap.ssid, s_ssid, ssid_len);
    wifi_cfg.ap.ssid_len = ssid_len;
    strlcpy((char *)wifi_cfg.ap.password, cfg.ap_password, sizeof(wifi_cfg.ap.password));
    wifi_cfg.ap.channel = CONFIG_GREENHOUSE_AP_CHANNEL;
    wifi_cfg.ap.max_connection = CONFIG_GREENHOUSE_AP_MAX_CONN;
    wifi_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

#if CONFIG_GREENHOUSE_DIAG_AP_OPEN
    ESP_LOGW(TAG, "diagnostic build: access point is OPEN, no password");
    wifi_cfg.ap.authmode = WIFI_AUTH_OPEN;
    wifi_cfg.ap.password[0] = '\0';
#endif

    /* config_save() refuses a passphrase of 1 to 7 characters, so an empty one
     * here is a deliberate open network rather than a typo. */
    if (wifi_cfg.ap.password[0] == '\0') {
        ESP_LOGW(TAG, "no AP password set, starting an open network");
        wifi_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    s_state = recovery ? GH_WIFI_STATE_RECOVERY : GH_WIFI_STATE_AP;
    s_clients = 0;
    note_ip(TCPIP_ADAPTER_IF_AP);

    if (recovery) {
        s_idle_deadline = esp_timer_get_time() +
                          (int64_t)CONFIG_GREENHOUSE_RECOVERY_AP_IDLE_S * 1000000;
        ESP_LOGW(TAG, "recovery SSID %s up at %s, restarting in %ds if nobody connects",
                 s_ssid, s_ip, CONFIG_GREENHOUSE_RECOVERY_AP_IDLE_S);
    } else {
        ESP_LOGI(TAG, "SSID %s up, browse to %s", s_ssid, s_ip);
    }

    return ESP_OK;
}

static esp_err_t start_sta(const greenhouse_config_t *cfg)
{
    strlcpy(s_ssid, cfg->sta_ssid, sizeof(s_ssid));

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, cfg->sta_ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, cfg->sta_password, sizeof(wifi_cfg.sta.password));

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_cfg);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * Hostname before start, so the first DHCP request already carries it. It
     * is how the device is found again on a network where its address is handed
     * out rather than known: the router's client list shows the AP name.
     */
    char host[GREENHOUSE_SSID_MAX];
    if (ap_name(cfg, host, sizeof(host)) == ESP_OK) {
        tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, host);
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    s_state = GH_WIFI_STATE_STA;
    ESP_LOGI(TAG, "joining %s, %d attempts before falling back to a recovery AP",
             s_ssid, CONFIG_GREENHOUSE_STA_CONNECT_ATTEMPTS);

    return ESP_OK;
}

/*
 * Whether a scanned name can be shown and stored as text. Broadcast SSIDs are
 * arbitrary bytes: an empty one is a hidden network, control characters would
 * have to be escaped everywhere the name travels, and a name that is not valid
 * UTF-8 would corrupt the JSON the page parses. Anything rejected here can
 * still be joined by typing it.
 */
static bool ssid_printable(const char *s)
{
    size_t len = strnlen(s, GREENHOUSE_SSID_MAX);

    if (len == 0 || len >= GREENHOUSE_SSID_MAX) {
        return false;
    }

    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)s[i];

        if (c < 0x20 || c == 0x7f) {
            return false;
        }

        /* ASCII, then the three multi-byte UTF-8 forms. Overlong encodings,
         * surrogates and out-of-range code points are all refused. */
        size_t extra;
        uint32_t cp;
        if (c < 0x80) {
            i++;
            continue;
        } else if ((c & 0xe0) == 0xc0) {
            extra = 1;
            cp = c & 0x1f;
        } else if ((c & 0xf0) == 0xe0) {
            extra = 2;
            cp = c & 0x0f;
        } else if ((c & 0xf8) == 0xf0) {
            extra = 3;
            cp = c & 0x07;
        } else {
            return false;
        }

        /* The continuation bytes have to be inside the string: the last one
         * is at i + extra, so that index must still be below len. */
        if (i + extra >= len) {
            return false;
        }

        for (size_t k = 1; k <= extra; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xc0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3f);
        }

        if ((extra == 1 && cp < 0x80) ||
            (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000) ||
            cp > 0x10ffff ||
            (cp >= 0xd800 && cp <= 0xdfff)) {
            return false;
        }

        i += extra + 1;
    }

    return true;
}

esp_err_t wifi_scan(wifi_scan_result_t *out, size_t max, size_t *found)
{
    if (out == NULL || found == NULL || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *found = 0;

    wifi_mode_t mode;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * Scanning needs a station interface. Serving only an access point there
     * is none, so add one for the duration and take it away again: leaving the
     * device in APSTA would have it answering as a station it is not using.
     */
    bool added_sta = (mode == WIFI_MODE_AP);
    if (added_sta) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            return err;
        }
    }

    wifi_scan_config_t scan = {0};
    scan.show_hidden = false;
    scan.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    /* Per channel. Thirteen channels at 120 ms is under two seconds, which is
     * about as long as a client on our own access point will tolerate the link
     * stalling. */
    scan.scan_time.active.min = 60;
    scan.scan_time.active.max = 120;

    err = esp_wifi_scan_start(&scan, true);

    uint16_t n = 0;
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_num(&n);
    }

    if (err == ESP_OK && n > 0) {
        wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
        if (recs == NULL) {
            /* The driver holds the results until they are read out, so read
             * and discard rather than leaking them until the next scan. */
            uint16_t none = 0;
            esp_wifi_scan_get_ap_records(&none, NULL);
            err = ESP_ERR_NO_MEM;
        } else {
            uint16_t got = n;
            err = esp_wifi_scan_get_ap_records(&got, recs);

            for (uint16_t i = 0; err == ESP_OK && i < got; i++) {
                char ssid[GREENHOUSE_SSID_MAX];
                strlcpy(ssid, (const char *)recs[i].ssid, sizeof(ssid));

                if (!ssid_printable(ssid)) {
                    continue;
                }

                /* One name, one entry: repeaters and mesh nodes share an SSID,
                 * and the list is for choosing a network, not a radio. */
                size_t at = *found;
                for (size_t k = 0; k < *found; k++) {
                    if (strcmp(out[k].ssid, ssid) == 0) {
                        at = k;
                        break;
                    }
                }

                if (at < *found) {
                    if (recs[i].rssi > out[at].rssi) {
                        out[at].rssi = recs[i].rssi;
                        out[at].secure = recs[i].authmode != WIFI_AUTH_OPEN;
                    }
                    continue;
                }

                if (*found == max) {
                    continue;
                }

                strlcpy(out[*found].ssid, ssid, sizeof(out[*found].ssid));
                out[*found].rssi = recs[i].rssi;
                out[*found].secure = recs[i].authmode != WIFI_AUTH_OPEN;
                (*found)++;
            }

            free(recs);
        }
    }

    if (added_sta) {
        esp_wifi_set_mode(WIFI_MODE_AP);
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Strongest first: insertion sort, since the list is at most sixteen. */
    for (size_t i = 1; i < *found; i++) {
        wifi_scan_result_t key = out[i];
        size_t k = i;
        while (k > 0 && out[k - 1].rssi < key.rssi) {
            out[k] = out[k - 1];
            k--;
        }
        out[k] = key;
    }

    ESP_LOGI(TAG, "scan found %u networks", (unsigned)*found);
    return ESP_OK;
}

const char *wifi_ssid(void)
{
    return s_ssid;
}

const char *wifi_ip(void)
{
    return s_ip;
}

int wifi_client_count(void)
{
    return s_clients;
}

gh_wifi_state_t gh_wifi_state(void)
{
    return s_state;
}

const char *wifi_state_name(void)
{
    switch (s_state) {
    case GH_WIFI_STATE_STA:      return s_sta_is_up ? "sta" : "connecting";
    case GH_WIFI_STATE_RECOVERY: return "recovery";
    default:                  return "ap";
    }
}

esp_err_t wifi_start(void)
{
    tcpip_adapter_init();

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        return err;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    greenhouse_config_t cfg;
    config_get(&cfg);

    bool station = cfg.wifi_mode == GH_WIFI_MODE_STA && cfg.sta_ssid[0] != '\0';

    if (cfg.wifi_mode == GH_WIFI_MODE_STA && !station) {
        /* config_save() rejects this combination, so reaching it means a blob
         * from elsewhere. The access point is the safe reading. */
        ESP_LOGW(TAG, "station mode configured with no network name, serving the access point");
    }

    if (station) {
        /* The supervisor only has work to do when a station attempt can fail. */
        if (xTaskCreate(supervisor_task, "wifi_sup", 3072, NULL, 4, &s_supervisor) != pdPASS) {
            ESP_LOGE(TAG, "could not start the supervisor task, serving the access point");
            station = false;
        }
    }

    err = station ? start_sta(&cfg) : start_ap(false);
    if (err != ESP_OK) {
        return err;
    }

#if CONFIG_GREENHOUSE_DIAG_WIFI_PROBES
    /* Probe requests are masked off by default. */
    esp_wifi_set_event_mask(0);
    ESP_LOGW(TAG, "diagnostic build: logging probe requests");
#endif

    return ESP_OK;
}
