#include "history.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static const char *TAG = "history";

#define CAPACITY CONFIG_GREENHOUSE_HISTORY_SAMPLES
#define INTERVAL_S CONFIG_GREENHOUSE_HISTORY_INTERVAL_S

static SemaphoreHandle_t s_lock;

/* Statically allocated so the memory is guaranteed at boot rather than
 * competing with Wi-Fi buffers for heap later. */
static history_sample_t s_ring[CAPACITY];
static size_t s_head;   /* index of the next write */
static size_t s_count;  /* samples held, <= CAPACITY */

static int64_t s_bucket_start_us;
static int32_t s_sum_temp;
static int32_t s_sum_hum;
static uint32_t s_sum_n;
static bool s_bucket_fan;
static bool s_fan_now;
static int64_t s_last_commit_us;

esp_err_t history_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_bucket_start_us = esp_timer_get_time();

    ESP_LOGI(TAG, "%d samples at %d s = %d h span, %u bytes",
             CAPACITY, INTERVAL_S, (CAPACITY * INTERVAL_S) / 3600,
             (unsigned)sizeof(s_ring));

    return ESP_OK;
}

void history_note_fan(bool on)
{
    if (s_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_fan_now = on;
    if (on) {
        s_bucket_fan = true;
    }
    xSemaphoreGive(s_lock);
}

static void commit_locked(void)
{
    history_sample_t s = {0};

    if (s_sum_n > 0) {
        /* Round to nearest rather than truncating; the bias is visible on the
         * chart, and at DHT11 resolution it is a whole degree. */
        s.temperature_dc = (int16_t)((s_sum_temp + (int32_t)s_sum_n / 2) / (int32_t)s_sum_n);
        s.humidity_dpct = (int16_t)((s_sum_hum + (int32_t)s_sum_n / 2) / (int32_t)s_sum_n);
        s.flags |= HISTORY_FLAG_VALID;
    }

    if (s_bucket_fan) {
        s.flags |= HISTORY_FLAG_FAN;
    }

    s_ring[s_head] = s;
    s_head = (s_head + 1) % CAPACITY;
    if (s_count < CAPACITY) {
        s_count++;
    }

    s_sum_temp = 0;
    s_sum_hum = 0;
    s_sum_n = 0;
    s_bucket_fan = s_fan_now;
    s_last_commit_us = esp_timer_get_time();
}

void history_add(bool valid, int temperature_dc, int humidity_dpct)
{
    if (s_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (valid) {
        s_sum_temp += temperature_dc;
        s_sum_hum += humidity_dpct;
        s_sum_n++;
    }

    int64_t now = esp_timer_get_time();
    const int64_t interval_us = (int64_t)INTERVAL_S * 1000000;

    /* A loop, not an if: if polling stalled for longer than one interval we
     * still want one sample per interval, with the skipped ones marked invalid
     * so the gap is visible instead of the timeline silently compressing. */
    while (now - s_bucket_start_us >= interval_us) {
        commit_locked();
        s_bucket_start_us += interval_us;
    }

    xSemaphoreGive(s_lock);
}

size_t history_count(void)
{
    if (s_lock == NULL) {
        return 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = s_count;
    xSemaphoreGive(s_lock);
    return n;
}

size_t history_copy(size_t from, size_t max, history_sample_t *out)
{
    if (s_lock == NULL || out == NULL || max == 0) {
        return 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    size_t n = 0;
    if (from < s_count) {
        n = s_count - from;
        if (n > max) {
            n = max;
        }

        /* Oldest sample sits at head when the ring has wrapped. */
        size_t oldest = (s_count == CAPACITY) ? s_head : 0;
        for (size_t i = 0; i < n; i++) {
            out[i] = s_ring[(oldest + from + i) % CAPACITY];
        }
    }

    xSemaphoreGive(s_lock);
    return n;
}

uint32_t history_interval_s(void)
{
    return INTERVAL_S;
}

uint32_t history_age_s(void)
{
    if (s_lock == NULL) {
        return 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t last = s_last_commit_us;
    xSemaphoreGive(s_lock);

    if (last == 0) {
        return 0;
    }

    return (uint32_t)((esp_timer_get_time() - last) / 1000000);
}
