#include "config.h"

#include <string.h>
#include <stdbool.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"

#define NVS_NAMESPACE "greenhouse"
#define NVS_KEY       "config"

static const char *TAG = "config";

static SemaphoreHandle_t s_lock;
static greenhouse_config_t s_cfg;

/*
 * Bounds come from the control model in docs/DESIGN.md. Anything outside these
 * is refused rather than clamped: silently altering a value the user typed hides
 * the mistake from them.
 */
typedef struct {
    const char *name;
    long min;
    long max;
} bound_t;

static const bound_t BOUND_TEMP        = {"temp_threshold_c", 0, 50};
static const bound_t BOUND_HUMIDITY    = {"humidity_threshold_pct", 20, 95};
static const bound_t BOUND_START_DELAY = {"start_delay_s", 0, 3600};
static const bound_t BOUND_MAX_FAN     = {"max_fan_duration_s", 30, 7200};
static const bound_t BOUND_GRACE       = {"grace_period_s", 0, 7200};
static const bound_t BOUND_POLL        = {"sensor_poll_interval_s", 2, 60};
static const bound_t BOUND_FAN_MODE    = {"fan_mode", FAN_MODE_OFF, FAN_MODE_ON};

void config_defaults(greenhouse_config_t *out)
{
    memset(out, 0, sizeof(*out));

    out->version = GREENHOUSE_CONFIG_VERSION;
    out->temp_threshold_c = 30;
    out->humidity_threshold_pct = 80;
    out->start_delay_s = 120;
    out->max_fan_duration_s = 900;
    out->grace_period_s = 300;
    out->sensor_poll_interval_s = CONFIG_GREENHOUSE_SENSOR_POLL_INTERVAL_S;
    out->relay_active_low = false;
    out->temp_enabled = true;
    out->humidity_enabled = true;
    out->fan_mode = FAN_MODE_AUTO;
    out->trigger_direction = GH_TRIGGER_ABOVE;
    strlcpy(out->device_name, "Fan", sizeof(out->device_name));
    out->ap_ssid[0] = '\0';  /* derive from the MAC */

    /*
     * Factory state is our own access point with the compiled-in password. It
     * is the only configuration that can be reached without knowing anything
     * about the site, which is what makes it a usable recovery target: the
     * password is printed in the documentation and on the device label.
     */
    out->wifi_mode = GH_WIFI_MODE_AP;
    strlcpy(out->ap_password, CONFIG_GREENHOUSE_AP_PASSWORD, sizeof(out->ap_password));
    out->sta_ssid[0] = '\0';
    out->sta_password[0] = '\0';
}

/*
 * The name is emitted inside a JSON string and rendered in the web UI. Rejecting
 * quotes and backslashes keeps the JSON emission a plain snprintf with no
 * escaping, and control characters are refused so the value cannot break the
 * response. Bytes above 0x7e are allowed through so UTF-8 names work; the page
 * renders the value with textContent, never innerHTML, so markup in it is inert.
 */
static bool name_ok(const char *n)
{
    size_t len = strnlen(n, GREENHOUSE_DEVICE_NAME_MAX);

    if (len == 0 || len >= GREENHOUSE_DEVICE_NAME_MAX) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)n[i];
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\') {
            return false;
        }
    }

    return true;
}

/*
 * An SSID is opaque bytes on the air, but ours is also emitted inside a JSON
 * string, so the same rules as the device name apply. Empty is handled by the
 * caller: for the AP it means "derive from the MAC", for the station it is
 * only legal when station mode is not selected.
 */
static bool ssid_ok(const char *s)
{
    size_t len = strnlen(s, GREENHOUSE_SSID_MAX);

    if (len >= GREENHOUSE_SSID_MAX) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\') {
            return false;
        }
    }

    return true;
}

/*
 * A WPA2 passphrase is 8 to 63 printable ASCII characters. Empty is allowed and
 * means an open network; lengths between 1 and 7 are refused rather than
 * quietly widened to open, which would turn a typo into an unprotected relay.
 */
static bool password_ok(const char *s)
{
    size_t len = strnlen(s, GREENHOUSE_PASSWORD_MAX);

    if (len >= GREENHOUSE_PASSWORD_MAX) {
        return false;
    }
    if (len == 0) {
        return true;
    }
    if (len < GREENHOUSE_PASSWORD_MIN) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c >= 0x7f) {
            return false;
        }
    }

    return true;
}

/*
 * Version 1 layout, kept so a config written before the enable flags existed can
 * be carried forward instead of silently reset to defaults. Never change this;
 * it describes what is already on devices.
 *
 * There is deliberately no version 2 migration. Version 2 exists only in
 * development builds and named three different layouts, so it cannot be
 * interpreted reliably; such a blob falls through to defaults.
 */
typedef struct {
    uint32_t version;
    int16_t temp_threshold_c;
    uint8_t humidity_threshold_pct;
    uint16_t start_delay_s;
    uint16_t max_fan_duration_s;
    uint16_t grace_period_s;
    uint8_t sensor_poll_interval_s;
    bool relay_active_low;
} config_v1_t;

static void migrate_v1(const config_v1_t *old, greenhouse_config_t *out)
{
    config_defaults(out);

    out->temp_threshold_c = old->temp_threshold_c;
    out->humidity_threshold_pct = old->humidity_threshold_pct;
    out->start_delay_s = old->start_delay_s;
    out->max_fan_duration_s = old->max_fan_duration_s;
    out->grace_period_s = old->grace_period_s;
    out->sensor_poll_interval_s = old->sensor_poll_interval_s;
    out->relay_active_low = old->relay_active_low;
    /* Both inputs enabled, which is how version 1 always behaved. */
}

/*
 * Version 3 layout: everything up to and including the AP name, before the
 * Wi-Fi settings moved out of Kconfig. Never change this; it describes what is
 * already on devices in the field.
 */
typedef struct {
    uint32_t version;
    int16_t temp_threshold_c;
    uint8_t humidity_threshold_pct;
    uint16_t start_delay_s;
    uint16_t max_fan_duration_s;
    uint16_t grace_period_s;
    uint8_t sensor_poll_interval_s;
    bool relay_active_low;
    bool temp_enabled;
    bool humidity_enabled;
    uint8_t fan_mode;
    char device_name[GREENHOUSE_DEVICE_NAME_MAX];
    char ap_ssid[GREENHOUSE_SSID_MAX];
} config_v3_t;

static void migrate_v3(const config_v3_t *old, greenhouse_config_t *out)
{
    config_defaults(out);

    out->temp_threshold_c = old->temp_threshold_c;
    out->humidity_threshold_pct = old->humidity_threshold_pct;
    out->start_delay_s = old->start_delay_s;
    out->max_fan_duration_s = old->max_fan_duration_s;
    out->grace_period_s = old->grace_period_s;
    out->sensor_poll_interval_s = old->sensor_poll_interval_s;
    out->relay_active_low = old->relay_active_low;
    out->temp_enabled = old->temp_enabled;
    out->humidity_enabled = old->humidity_enabled;
    out->fan_mode = old->fan_mode;
    memcpy(out->device_name, old->device_name, sizeof(out->device_name));
    memcpy(out->ap_ssid, old->ap_ssid, sizeof(out->ap_ssid));
    /* Wi-Fi keeps the defaults: version 3 ran the access point unconditionally
     * with the compiled-in password, which is exactly what they describe. */
}

/*
 * Version 4 layout: before the trigger direction, when the relay could only
 * switch on above the thresholds. Never change this; it describes what is
 * already on devices in the field.
 */
typedef struct {
    uint32_t version;
    int16_t temp_threshold_c;
    uint8_t humidity_threshold_pct;
    uint16_t start_delay_s;
    uint16_t max_fan_duration_s;
    uint16_t grace_period_s;
    uint8_t sensor_poll_interval_s;
    bool relay_active_low;
    bool temp_enabled;
    bool humidity_enabled;
    uint8_t fan_mode;
    char device_name[GREENHOUSE_DEVICE_NAME_MAX];
    char ap_ssid[GREENHOUSE_SSID_MAX];
    uint8_t wifi_mode;
    char ap_password[GREENHOUSE_PASSWORD_MAX];
    char sta_ssid[GREENHOUSE_SSID_MAX];
    char sta_password[GREENHOUSE_PASSWORD_MAX];
} config_v4_t;

static void migrate_v4(const config_v4_t *old, greenhouse_config_t *out)
{
    config_defaults(out);

    out->temp_threshold_c = old->temp_threshold_c;
    out->humidity_threshold_pct = old->humidity_threshold_pct;
    out->start_delay_s = old->start_delay_s;
    out->max_fan_duration_s = old->max_fan_duration_s;
    out->grace_period_s = old->grace_period_s;
    out->sensor_poll_interval_s = old->sensor_poll_interval_s;
    out->relay_active_low = old->relay_active_low;
    out->temp_enabled = old->temp_enabled;
    out->humidity_enabled = old->humidity_enabled;
    out->fan_mode = old->fan_mode;
    memcpy(out->device_name, old->device_name, sizeof(out->device_name));
    memcpy(out->ap_ssid, old->ap_ssid, sizeof(out->ap_ssid));
    out->wifi_mode = old->wifi_mode;
    memcpy(out->ap_password, old->ap_password, sizeof(out->ap_password));
    memcpy(out->sta_ssid, old->sta_ssid, sizeof(out->sta_ssid));
    memcpy(out->sta_password, old->sta_password, sizeof(out->sta_password));
    /* Direction keeps its default: version 4 only ever switched on above the
     * thresholds, which is what GH_TRIGGER_ABOVE means. */
}

static const char *validate(const greenhouse_config_t *c)
{
    const struct {
        const bound_t *b;
        long v;
    } checks[] = {
        {&BOUND_TEMP,        c->temp_threshold_c},
        {&BOUND_HUMIDITY,    c->humidity_threshold_pct},
        {&BOUND_START_DELAY, c->start_delay_s},
        {&BOUND_MAX_FAN,     c->max_fan_duration_s},
        {&BOUND_GRACE,       c->grace_period_s},
        {&BOUND_POLL,        c->sensor_poll_interval_s},
        {&BOUND_FAN_MODE,    c->fan_mode},
    };

    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
        if (checks[i].v < checks[i].b->min || checks[i].v > checks[i].b->max) {
            return checks[i].b->name;
        }
    }

    if (!name_ok(c->device_name)) {
        return "device_name";
    }

    /* Empty is legal and means "derive from the MAC"; anything else is held to
     * the same character rules as the device name. */
    if (!ssid_ok(c->ap_ssid)) {
        return "ap_ssid";
    }

    if (c->wifi_mode != GH_WIFI_MODE_AP && c->wifi_mode != GH_WIFI_MODE_STA) {
        return "wifi_mode";
    }

    if (c->trigger_direction != GH_TRIGGER_ABOVE &&
        c->trigger_direction != GH_TRIGGER_BELOW) {
        return "trigger_direction";
    }

    if (!password_ok(c->ap_password)) {
        return "ap_password";
    }

    if (!ssid_ok(c->sta_ssid)) {
        return "sta_ssid";
    }

    if (!password_ok(c->sta_password)) {
        return "sta_password";
    }

    /*
     * Selecting station mode with no network to join would strand the device in
     * the recovery access point on every boot. Refusing it here means the only
     * way into that state is a corrupt blob, which wifi_start() handles.
     */
    if (c->wifi_mode == GH_WIFI_MODE_STA && c->sta_ssid[0] == '\0') {
        return "sta_ssid";
    }

    return NULL;
}

static esp_err_t persist(const greenhouse_config_t *c)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(h, NVS_KEY, c, sizeof(*c));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

esp_err_t config_load(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    greenhouse_config_t loaded;
    bool use_defaults = false;
    bool migrated = false;

    /* Big enough for any known layout, so the stored length can be inspected
     * rather than having to match the current struct. Version 4 is the largest
     * of the old ones; the current struct is larger still. */
    uint8_t raw[sizeof(greenhouse_config_t) > sizeof(config_v4_t)
                ? sizeof(greenhouse_config_t) : sizeof(config_v4_t)];
    size_t len = sizeof(raw);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no stored config (%s), using defaults", esp_err_to_name(err));
        use_defaults = true;
    } else {
        err = nvs_get_blob(h, NVS_KEY, raw, &len);
        nvs_close(h);

        uint32_t stored_version = 0;
        if (err == ESP_OK && len >= sizeof(uint32_t)) {
            memcpy(&stored_version, raw, sizeof(stored_version));
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "stored config unreadable, using defaults");
            use_defaults = true;
        } else if (stored_version == GREENHOUSE_CONFIG_VERSION &&
                   len == sizeof(greenhouse_config_t)) {
            memcpy(&loaded, raw, sizeof(loaded));
        } else if (stored_version == 1 && len == sizeof(config_v1_t)) {
            config_v1_t old;
            memcpy(&old, raw, sizeof(old));
            migrate_v1(&old, &loaded);
            migrated = true;
            ESP_LOGI(TAG, "migrated stored config from version 1");
        } else if (stored_version == 4 && len == sizeof(config_v4_t)) {
            config_v4_t old;
            memcpy(&old, raw, sizeof(old));
            migrate_v4(&old, &loaded);
            migrated = true;
            ESP_LOGI(TAG, "migrated stored config from version 4");
        } else if (stored_version == 3 && len == sizeof(config_v3_t)) {
            config_v3_t old;
            memcpy(&old, raw, sizeof(old));
            migrate_v3(&old, &loaded);
            migrated = true;
            ESP_LOGI(TAG, "migrated stored config from version 3");
        } else {
            ESP_LOGW(TAG, "stored config is version %u, %u bytes; using defaults",
                     stored_version, (unsigned)len);
            use_defaults = true;
        }

        if (!use_defaults) {
            /* Stored values are validated too. A config written by an older
             * build, or corrupted in place, must not reach the controller. */
            const char *bad = validate(&loaded);
            if (bad != NULL) {
                ESP_LOGW(TAG, "stored config field %s out of bounds, using defaults", bad);
                use_defaults = true;
            }
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (use_defaults) {
        config_defaults(&s_cfg);
    } else {
        s_cfg = loaded;
    }
    greenhouse_config_t snapshot = s_cfg;
    xSemaphoreGive(s_lock);

    if (use_defaults || migrated) {
        esp_err_t werr = persist(&snapshot);
        if (werr != ESP_OK) {
            ESP_LOGE(TAG, "could not write defaults: %s", esp_err_to_name(werr));
        }
    }

    ESP_LOGI(TAG, "temp>%d C%s, humidity>%d %%RH%s, start %ds, max %ds, grace %ds, poll %ds",
             snapshot.temp_threshold_c, snapshot.temp_enabled ? "" : " (disabled)",
             snapshot.humidity_threshold_pct, snapshot.humidity_enabled ? "" : " (disabled)",
             snapshot.start_delay_s, snapshot.max_fan_duration_s,
             snapshot.grace_period_s, snapshot.sensor_poll_interval_s);

    if (snapshot.fan_mode != FAN_MODE_AUTO) {
        ESP_LOGW(TAG, "fan mode is forced %s, not automatic",
                 snapshot.fan_mode == FAN_MODE_ON ? "ON" : "OFF");
    }

    if (snapshot.fan_mode == FAN_MODE_AUTO &&
        !snapshot.temp_enabled && !snapshot.humidity_enabled) {
        ESP_LOGW(TAG, "both inputs disabled: the fan will not start automatically");
    }

    return ESP_OK;
}

void config_get(greenhouse_config_t *out)
{
    if (out == NULL) {
        return;
    }

    if (s_lock == NULL) {
        config_defaults(out);
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

esp_err_t config_save(const greenhouse_config_t *in, const char **err_field)
{
    if (in == NULL || s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    greenhouse_config_t candidate = *in;
    candidate.version = GREENHOUSE_CONFIG_VERSION;

    const char *bad = validate(&candidate);
    if (bad != NULL) {
        if (err_field != NULL) {
            *err_field = bad;
        }
        return ESP_ERR_INVALID_ARG;
    }

    /* Persist before publishing, so a failed write never leaves the running
     * configuration disagreeing with what is stored. */
    esp_err_t err = persist(&candidate);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = candidate;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "saved: temp>%d C, humidity>%d %%RH, start %ds, max %ds, grace %ds, poll %ds",
             candidate.temp_threshold_c, candidate.humidity_threshold_pct,
             candidate.start_delay_s, candidate.max_fan_duration_s,
             candidate.grace_period_s, candidate.sensor_poll_interval_s);

    return ESP_OK;
}

esp_err_t config_reset(void)
{
    greenhouse_config_t defaults;
    config_defaults(&defaults);
    return config_save(&defaults, NULL);
}
