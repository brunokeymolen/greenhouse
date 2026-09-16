#include "wifi_ap.h"

#include <string.h>

#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include "tcpip_adapter.h"

static const char *TAG = "wifi_ap";

static char s_ssid[GREENHOUSE_AP_SSID_MAX];
static int s_clients;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        s_clients++;
        ESP_LOGI(TAG, "station " MACSTR " joined, AID=%d, %d connected",
                 MAC2STR(event->mac), event->aid, s_clients);
#if CONFIG_GREENHOUSE_DIAG_WIFI_PROBES
    } else if (event_id == WIFI_EVENT_AP_PROBEREQRECVED) {
        /* Every nearby device probes, so rate limit rather than flooding the
         * console and disturbing the sensor timing. */
        static int64_t last_us;
        int64_t now = esp_timer_get_time();
        if (now - last_us > 500000) {
            last_us = now;
            wifi_event_ap_probe_req_rx_t *event = (wifi_event_ap_probe_req_rx_t *)event_data;
            ESP_LOGI(TAG, "probe from " MACSTR ", rssi %d", MAC2STR(event->mac), event->rssi);
        }
#endif
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        if (s_clients > 0) {
            s_clients--;
        }
        ESP_LOGI(TAG, "station " MACSTR " left, AID=%d, %d connected",
                 MAC2STR(event->mac), event->aid, s_clients);
    }
}

const char *wifi_ap_ssid(void)
{
    return s_ssid;
}

int wifi_ap_client_count(void)
{
    return s_clients;
}

esp_err_t wifi_ap_start(void)
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

    greenhouse_config_t cfg;
    config_get(&cfg);

    if (cfg.ap_ssid[0] != '\0') {
        strlcpy(s_ssid, cfg.ap_ssid, sizeof(s_ssid));
    } else {
        /* No name configured: derive one from the radio's own MAC so two units
         * on the same bench never collide. */
        uint8_t mac[6] = {0};
        err = esp_wifi_get_mac(ESP_IF_WIFI_AP, mac);
        if (err != ESP_OK) {
            return err;
        }
        snprintf(s_ssid, sizeof(s_ssid), "Greenhouse-%02X%02X", mac[4], mac[5]);
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
    strlcpy((char *)wifi_cfg.ap.password, CONFIG_GREENHOUSE_AP_PASSWORD,
            sizeof(wifi_cfg.ap.password));
    wifi_cfg.ap.channel = CONFIG_GREENHOUSE_AP_CHANNEL;
    wifi_cfg.ap.max_connection = CONFIG_GREENHOUSE_AP_MAX_CONN;
    wifi_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

#if CONFIG_GREENHOUSE_DIAG_AP_OPEN
    ESP_LOGW(TAG, "diagnostic build: access point is OPEN, no password");
    wifi_cfg.ap.authmode = WIFI_AUTH_OPEN;
    wifi_cfg.ap.password[0] = '\0';
#endif

    /* WPA2 needs at least 8 characters; fall back to open rather than refusing
     * to start, so a misconfigured build is still reachable to be fixed. */
    if (strlen(CONFIG_GREENHOUSE_AP_PASSWORD) < 8) {
        ESP_LOGW(TAG, "AP password shorter than 8 characters, starting open network");
        wifi_cfg.ap.authmode = WIFI_AUTH_OPEN;
        wifi_cfg.ap.password[0] = '\0';
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

#if CONFIG_GREENHOUSE_DIAG_WIFI_PROBES
    /* Probe requests are masked off by default. */
    esp_wifi_set_event_mask(0);
    ESP_LOGW(TAG, "diagnostic build: logging probe requests");
#endif

    tcpip_adapter_ip_info_t ip_info;
    if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "SSID %s up, browse to " IPSTR, s_ssid, IP2STR(&ip_info.ip));
    } else {
        ESP_LOGI(TAG, "SSID %s up", s_ssid);
    }

    return ESP_OK;
}
