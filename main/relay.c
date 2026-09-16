#include "relay.h"

#include "config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "history.h"
#include "sdkconfig.h"

/* Hard ceiling on the manual test pulse, whatever a caller asks for. */
#define RELAY_TEST_MAX_S 5

static const char *TAG = "relay";

static SemaphoreHandle_t s_lock;
static bool s_on;
static esp_timer_handle_t s_test_timer;

/*
 * A manual test owns the output for its duration. The controller re-asserts its
 * decision every tick, which would otherwise cancel the pulse within a second,
 * so its requests are recorded but not driven while a test is running and are
 * applied when the pulse expires.
 */
static bool s_test_active;
static bool s_desired;

/*
 * Translate logical on/off to a pin level. Polarity is runtime configuration
 * rather than compile-time: which way round the carrier drives the relay is an
 * open question on this hardware, and being able to flip it from the web UI
 * during bring-up beats a reflash per attempt.
 */
static int level_for(bool on)
{
    greenhouse_config_t cfg;
    config_get(&cfg);
    if (cfg.relay_active_low) {
        return on ? 0 : 1;
    }
    return on ? 1 : 0;
}

static void drive_locked(bool on)
{
    s_on = on;
    gpio_set_level(CONFIG_GREENHOUSE_RELAY_GPIO, level_for(on));
    history_note_fan(on);
}

static void test_timer_cb(void *arg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_test_active = false;
    /* Hand the output back to whatever the controller last asked for, which is
     * not necessarily off. */
    drive_locked(s_desired);
    bool now = s_desired;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "test pulse expired, relay %s", now ? "ON" : "off");
}

esp_err_t relay_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /*
     * Preload the output register before enabling the driver, so the pin never
     * momentarily presents the wrong level as it becomes an output.
     */
    gpio_set_level(CONFIG_GREENHOUSE_RELAY_GPIO, level_for(false));

    gpio_config_t cfg = {
        .pin_bit_mask = (1U << CONFIG_GREENHOUSE_RELAY_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    drive_locked(false);
    xSemaphoreGive(s_lock);

    const esp_timer_create_args_t targs = {
        .callback = test_timer_cb,
        .name = "relay_test",
    };
    err = esp_timer_create(&targs, &s_test_timer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "test timer unavailable: %s", esp_err_to_name(err));
        s_test_timer = NULL;
    }

    greenhouse_config_t c;
    config_get(&c);
    ESP_LOGI(TAG, "GPIO%d, active %s, off at boot",
             CONFIG_GREENHOUSE_RELAY_GPIO, c.relay_active_low ? "low" : "high");

    /*
     * GPIO0 is also the boot strap and is held high by its pull-up until this
     * runs. On an active-high carrier the relay is therefore energised from
     * power-up until relay_init(), which is why it is called first in app_main.
     */
    return ESP_OK;
}

esp_err_t relay_set(bool on)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    s_desired = on;

    if (s_test_active) {
        /* Remembered, applied when the pulse expires. */
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    bool changed = (s_on != on);
    drive_locked(on);
    xSemaphoreGive(s_lock);

    if (changed) {
        ESP_LOGI(TAG, "relay %s", on ? "ON" : "off");
    }

    return ESP_OK;
}

bool relay_is_on(void)
{
    if (s_lock == NULL) {
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool on = s_on;
    xSemaphoreGive(s_lock);
    return on;
}

void relay_refresh(void)
{
    if (s_lock == NULL) {
        return;
    }

    /* Re-drive the current logical state, so a polarity change from the web UI
     * takes effect immediately instead of at the next controller decision. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    drive_locked(s_on);
    xSemaphoreGive(s_lock);
}

esp_err_t relay_test_pulse(int seconds)
{
    if (s_lock == NULL || s_test_timer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (seconds < 1) {
        seconds = 1;
    }
    if (seconds > RELAY_TEST_MAX_S) {
        seconds = RELAY_TEST_MAX_S;
    }

    esp_timer_stop(s_test_timer);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_test_active = true;
    drive_locked(true);
    xSemaphoreGive(s_lock);

    /* Arm the off-timer after switching on, never before: if arming fails the
     * relay must not be left latched. */
    esp_err_t err = esp_timer_start_once(s_test_timer, (uint64_t)seconds * 1000000);
    if (err != ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_test_active = false;
        drive_locked(s_desired);
        xSemaphoreGive(s_lock);
        return err;
    }

    ESP_LOGI(TAG, "test pulse, %d s", seconds);
    return ESP_OK;
}

int relay_test_max_s(void)
{
    return RELAY_TEST_MAX_S;
}
