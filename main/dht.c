/*
 * Copyright (c) 2026 Bruno Keymolen
 * SPDX-License-Identifier: MIT
 */

#include "dht.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"
#include "sdkconfig.h"

/*
 * Host start pulse.
 *
 * The DHT11 wants at least 18 ms. That is a tick delay, and vTaskDelay(n) only
 * guarantees (n-1) whole ticks because the first boundary is partial, so at
 * CONFIG_FREERTOS_HZ=100 we ask for 30 ms to be sure of clearing 18.
 *
 * The DHT22 wants only about 1 ms, which is below the 10 ms tick entirely, so it
 * uses a busy-wait instead. Being preempted there only lengthens the pulse,
 * which the part tolerates, so this deliberately does not disable interrupts:
 * a millisecond of added interrupt latency every poll is the worse trade.
 */
#if CONFIG_GREENHOUSE_SENSOR_DHT22
#define START_LOW_US 1200
#else
#define START_LOW_MS 30
#endif

/* The sensor answers with 80 us low + 80 us high, so 200 us is generous. */
#define HANDSHAKE_TIMEOUT_US 200

/* Each bit is ~50 us low then 26-28 us (zero) or ~70 us (one) high. */
#define BIT_LOW_TIMEOUT_US 120
#define BIT_HIGH_TIMEOUT_US 150

/* Anything longer than this in the high phase is a one. */
#define BIT_ONE_THRESHOLD_US 45

/*
 * Minimum spacing between conversions. The DHT11 manages one per second and the
 * DHT22 one per two; polling nearer the limit returns partly stale frames on
 * both, so two seconds covers each part.
 */
#define MIN_INTERVAL_US 2000000

static gpio_num_t s_pin = GPIO_NUM_MAX;
static int64_t s_last_read_us;

static uint8_t s_last_bytes[5];
static int s_last_pulse_us[40];

void dht_last_frame(uint8_t bytes[5], int pulse_us[40])
{
    memcpy(bytes, s_last_bytes, sizeof(s_last_bytes));
    memcpy(pulse_us, s_last_pulse_us, sizeof(s_last_pulse_us));
}

const char *dht_model(void)
{
#if CONFIG_GREENHOUSE_SENSOR_DHT22
    return "DHT22";
#else
    return "DHT11";
#endif
}

/*
 * Rated measuring ranges from the datasheets. The DHT11's narrow humidity range
 * matters for this application: a greenhouse crosses 90 %RH exactly when
 * ventilation is wanted, which is past what that part can measure.
 */
#if CONFIG_GREENHOUSE_SENSOR_DHT22
int dht_humidity_min_dpct(void) { return 0; }
int dht_humidity_max_dpct(void) { return 1000; }
int dht_temperature_min_dc(void) { return -400; }
int dht_temperature_max_dc(void) { return 800; }
#else
int dht_humidity_min_dpct(void) { return 200; }
int dht_humidity_max_dpct(void) { return 900; }
int dht_temperature_min_dc(void) { return 0; }
int dht_temperature_max_dc(void) { return 500; }
#endif

esp_err_t dht_init(gpio_num_t pin)
{
    /*
     * Open drain with the internal pull-up: writing 0 drives the line low,
     * writing 1 releases it. On the ESP8266 the input buffer stays connected in
     * output mode, so the same pin can be read back without ever switching
     * direction. That keeps the timing-critical section free of gpio_config(),
     * which is slow and logs on every call.
     *
     * The internal pull-up is weak. Keep the external 4.7k-10k resistor from the
     * design document; without it the rising edges are too slow to decode.
     */
    gpio_config_t cfg = {
        .pin_bit_mask = (1U << pin),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    s_pin = pin;
    s_last_read_us = 0;

    /* Idle high. */
    return gpio_set_level(s_pin, 1);
}

/*
 * Spin until the line reaches `level`, returning how long that took in
 * microseconds, or -1 on timeout.
 */
static int wait_for_level(int level, int timeout_us)
{
    int64_t start = esp_timer_get_time();

    while (gpio_get_level(s_pin) != level) {
        if (esp_timer_get_time() - start > timeout_us) {
            return -1;
        }
    }

    return (int)(esp_timer_get_time() - start);
}

esp_err_t dht_read(dht_reading_t *out)
{
    if (s_pin == GPIO_NUM_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Respect the minimum conversion interval, otherwise we read stale bits. */
    if (s_last_read_us != 0) {
        int64_t since = esp_timer_get_time() - s_last_read_us;
        if (since < MIN_INTERVAL_US) {
            vTaskDelay(pdMS_TO_TICKS((MIN_INTERVAL_US - since) / 1000 + 1));
        }
    }

    /* Start pulse, then release the line and let the pull-up take it high. */
    gpio_set_level(s_pin, 0);
#if CONFIG_GREENHOUSE_SENSOR_DHT22
    ets_delay_us(START_LOW_US);
#else
    vTaskDelay(pdMS_TO_TICKS(START_LOW_MS));
#endif
    gpio_set_level(s_pin, 1);

    uint8_t data[5] = {0};
    int pulse_us[40] = {0};
    esp_err_t err = ESP_OK;

    /*
     * Interrupts stay off for the whole frame, about 5 ms. A preempted read
     * decodes garbage, so there is no gentler option with a bit-banged sensor.
     * Wi-Fi tolerates this at the poll intervals we use, but it is the reason
     * the poll interval has a floor.
     */
    portENTER_CRITICAL();

    if (wait_for_level(0, HANDSHAKE_TIMEOUT_US) < 0 ||
        wait_for_level(1, HANDSHAKE_TIMEOUT_US) < 0 ||
        wait_for_level(0, HANDSHAKE_TIMEOUT_US) < 0) {
        err = ESP_ERR_TIMEOUT;
    } else {
        for (int i = 0; i < 40; i++) {
            if (wait_for_level(1, BIT_LOW_TIMEOUT_US) < 0) {
                err = ESP_ERR_TIMEOUT;
                break;
            }

            int high_us = wait_for_level(0, BIT_HIGH_TIMEOUT_US);
            if (high_us < 0) {
                err = ESP_ERR_TIMEOUT;
                break;
            }

            pulse_us[i] = high_us;

            data[i / 8] <<= 1;
            if (high_us > BIT_ONE_THRESHOLD_US) {
                data[i / 8] |= 1;
            }
        }
    }

    portEXIT_CRITICAL();

    memcpy(s_last_bytes, data, sizeof(s_last_bytes));
    memcpy(s_last_pulse_us, pulse_us, sizeof(s_last_pulse_us));

    s_last_read_us = esp_timer_get_time();

    if (err != ESP_OK) {
        return err;
    }

    /*
     * An all-zero frame satisfies the checksum trivially (0+0+0+0 == 0), so the
     * checksum alone cannot reject it. The sensor sends this when it has been
     * reset and has no conversion yet, typically after a supply brownout. It
     * decodes as 0 C and 0 %RH, which a fan controller would read as "cold and
     * dry, do not ventilate" -- the most dangerous possible misreading.
     */
    if (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 0 && data[4] == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t sum = data[0] + data[1] + data[2] + data[3];
    if (sum != data[4]) {
        return ESP_ERR_INVALID_CRC;
    }

#if CONFIG_GREENHOUSE_SENSOR_DHT22
    /*
     * DHT22: two 16-bit big-endian values in tenths. The temperature's sign
     * lives in the top bit of the high byte, so it is sign-and-magnitude rather
     * than two's complement.
     */
    out->humidity_dpct = (int16_t)(((uint16_t)data[0] << 8) | data[1]);

    uint16_t raw_t = ((uint16_t)data[2] << 8) | data[3];
    int16_t t = (int16_t)(raw_t & 0x7fff);
    out->temperature_dc = (raw_t & 0x8000) ? (int16_t)-t : t;
#else
    /*
     * DHT11: integer part and a decimal part that is almost always zero.
     * Scaled to tenths so callers need not know which part is fitted.
     */
    out->humidity_dpct = (int16_t)data[0] * 10 + (int16_t)(data[1] & 0x0f);

    int16_t t = (int16_t)data[2] * 10 + (int16_t)(data[3] & 0x7f);
    out->temperature_dc = (data[3] & 0x80) ? (int16_t)-t : t;
#endif

    /* Physically impossible: reject outright. */
    if (out->humidity_dpct < 0 || out->humidity_dpct > 1000) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}
