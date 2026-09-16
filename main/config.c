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
    strlcpy(out->device_name, "Fan", sizeof(out->device_name));
    out->ap_ssid[0] = '\0';  /* derive from the MAC */
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
    if (c->ap_ssid[0] != '\0') {
        size_t len = strnlen(c->ap_ssid, GREENHOUSE_AP_SSID_MAX);
        if (len >= GREENHOUSE_AP_SSID_MAX) {
            return "ap_ssid";
        }
        for (size_t i = 0; i < len; i++) {
            unsigned char ch = (unsigned char)c->ap_ssid[i];
            if (ch < 0x20 || ch == 0x7f || ch == '"' || ch == '\\') {
                return "ap_ssid";
            }
        }
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

    /* Read into a buffer big enough for any known layout, so the stored length
     * can be inspected rather than having to match the current struct. */
    uint8_t raw[sizeof(greenhouse_config_t) > sizeof(config_v1_t)
                ? sizeof(greenhouse_config_t) : sizeof(config_v1_t)];
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
