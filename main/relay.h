/*
 * Relay output with configurable polarity and a fail-safe off state.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Configure the relay pin and drive it to the off state.
 *
 * Call this as early as possible in app_main(). GPIO0 is also the boot strap and
 * idles high through its pull-up, so on an active-high carrier the relay is
 * energised from power-up until this runs.
 */
esp_err_t relay_init(void);

/*
 * Drive the relay. Cancels any test pulse in progress.
 */
esp_err_t relay_set(bool on);

bool relay_is_on(void);

/*
 * Turn the relay on for at most relay_test_max_s() seconds, then off again
 * automatically. Used for bring-up; never leaves the relay latched on.
 */
esp_err_t relay_test_pulse(int seconds);

/*
 * Re-drive the current logical state. Call after the relay polarity setting
 * changes so it takes effect without waiting for the next controller decision.
 */
void relay_refresh(void);

/*
 * The hard ceiling relay_test_pulse() clamps to.
 */
int relay_test_max_s(void);

#ifdef __cplusplus
}
#endif
