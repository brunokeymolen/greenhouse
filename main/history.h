/*
 * In-RAM ring buffer of downsampled sensor history, for the chart in the web UI.
 *
 * Deliberately not persisted. Writing a sample to NVS every few minutes would
 * wear the flash out on a device meant to run for whole seasons, so history is
 * lost on reboot and that is the accepted trade.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HISTORY_FLAG_VALID 0x01  /* the bucket contained at least one good read */
#define HISTORY_FLAG_FAN   0x02  /* the fan ran at some point during the bucket */

typedef struct {
    int16_t temperature_dc;  /* tenths of a degree Celsius */
    int16_t humidity_dpct;   /* tenths of a percent RH */
    uint8_t flags;
} history_sample_t;

esp_err_t history_init(void);

/*
 * Feed one sensor poll. Polls are averaged into fixed-length buckets; a bucket
 * is committed to the ring when its interval elapses. Invalid polls are counted
 * but not averaged, so a bucket with no good reads commits without
 * HISTORY_FLAG_VALID and the chart draws a gap rather than a fabricated zero.
 */
void history_add(bool valid, int temperature_dc, int humidity_dpct);

/*
 * Tell history whether the fan is currently running. Any "on" seen during a
 * bucket marks the whole bucket, which is what the shaded blocks represent.
 * Call from the relay layer when it exists.
 */
void history_note_fan(bool on);

/*
 * Number of samples held, oldest first.
 */
size_t history_count(void);

/*
 * Copy at most `max` samples starting at chronological index `from` into `out`.
 * Returns how many were copied. Copying a window at a time keeps the lock hold
 * short and the caller's stack small while streaming a large response.
 */
size_t history_copy(size_t from, size_t max, history_sample_t *out);

/*
 * Seconds between committed samples, and seconds since the newest one.
 */
uint32_t history_interval_s(void);
uint32_t history_age_s(void);

#ifdef __cplusplus
}
#endif
