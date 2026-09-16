/*
 * Bit-banged driver for the DHT11 and DHT22/AM2302.
 *
 * Both parts use the same single-wire protocol and the same pulse-width bit
 * encoding. They differ in the length of the host start pulse and in how the
 * 40 payload bits are interpreted, so the transport below is shared and only
 * those two things branch on CONFIG_GREENHOUSE_SENSOR_DHT22.
 *
 * Values are carried in tenths so the DHT22's 0.1 resolution survives; a DHT11
 * simply reports multiples of 10.
 */

#pragma once

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t temperature_dc;  /* tenths of a degree Celsius */
    int16_t humidity_dpct;   /* tenths of a percent relative humidity */
} dht_reading_t;

esp_err_t dht_init(gpio_num_t pin);

/*
 * Take one reading.
 *
 * Returns ESP_OK, ESP_ERR_TIMEOUT if the sensor did not respond or a pulse ran
 * long, ESP_ERR_INVALID_CRC on a checksum mismatch, or ESP_ERR_INVALID_RESPONSE
 * for an all-zero frame. Isolated failures are normal; retry rather than
 * treating one as a fault.
 *
 * Disables interrupts for roughly 5 ms while clocking in the frame.
 */
esp_err_t dht_read(dht_reading_t *out);

/*
 * The five raw bytes of the most recent frame, valid or not, plus the measured
 * high-pulse width of each of the 40 bits in microseconds. Diagnostics only:
 * this is how you tell a decode problem from a genuinely noisy sensor.
 */
void dht_last_frame(uint8_t bytes[5], int pulse_us[40]);

/*
 * "DHT11" or "DHT22", for logs and the web UI.
 */
const char *dht_model(void);

/*
 * The part's rated measuring range, in the same tenths units as a reading.
 * Readings outside it are reported but should not be trusted.
 */
int dht_humidity_min_dpct(void);
int dht_humidity_max_dpct(void);
int dht_temperature_min_dc(void);
int dht_temperature_max_dc(void);

#ifdef __cplusplus
}
#endif
