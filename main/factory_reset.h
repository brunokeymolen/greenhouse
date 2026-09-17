/*
 * Factory reset from the reset button.
 *
 * The ESP-01 carrier has exactly one button, wired to RST, and the header has
 * no pin to spare: GPIO0 drives the relay, GPIO2 the sensor, GPIO1 and GPIO3
 * are the UART. So a held button cannot be the trigger. RST is an asynchronous
 * hardware reset: while it is held the CPU is *in* reset and no code is running
 * to time it, and on release the chip boots with no record of how long it was
 * down. Holding the button for ten seconds is indistinguishable from tapping it.
 *
 * What is observable is how the boot was caused. Tapping the button several
 * times in a row leaves a trail: each boot reports ESP_RST_EXT and finds the
 * previous boot's count still in RTC memory, which survives a reset but not a
 * power cut. Enough taps close enough together is a deliberate act, and nothing
 * else produces that pattern -- a crash reports ESP_RST_PANIC, a hang
 * ESP_RST_WDT, and either breaks the chain.
 *
 * This is the only way back into a device whose owner has forgotten the
 * password, and it deliberately requires standing in front of it.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Count this boot, and restore factory defaults if enough resets have been
 * chained together.
 *
 * Call once, early in app_main(), after config_load() so there is a
 * configuration to overwrite, and after relay_init() so the relay is in its
 * off state before any confirmation clicks.
 */
void factory_reset_check(void);

/*
 * Whether this boot was the one that reset the configuration. Lets the web UI
 * confirm what happened; the device is otherwise silent about it.
 */
bool factory_reset_triggered(void);

#ifdef __cplusplus
}
#endif
