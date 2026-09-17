#include "factory_reset.h"

#include "config.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "relay.h"
#include "sdkconfig.h"

static const char *TAG = "factory";

/*
 * Chosen so an uninitialised RTC block is overwhelmingly unlikely to look like
 * a reset chain in progress. RTC memory holds whatever it held before across a
 * reset, and arbitrary bytes after a power cut.
 */
#define FR_MAGIC 0x47484652u  /* "GHFR" */

/*
 * RTC_NOINIT_ATTR places this in .rtc_noinit, a NOLOAD section in the RTC
 * segment at 0x60001200. Nothing in the startup path clears it, which is the
 * whole point: it has to survive the reset that is being counted.
 */
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    uint32_t count;
} s_chain;

static bool s_triggered;

/*
 * Ends the chain a short while after boot, so the count only ever reflects
 * taps in quick succession. Without this, five resets spread over a season
 * would eventually add up to a factory reset.
 */
static void window_expired_cb(void *arg)
{
    if (s_chain.count != 0) {
        ESP_LOGI(TAG, "reset chain expired");
        s_chain.count = 0;
    }
}

static void arm_window(void)
{
    static esp_timer_handle_t timer;

    if (timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = window_expired_cb,
            .name = "fr_window",
        };
        if (esp_timer_create(&args, &timer) != ESP_OK) {
            /* The chain would then never expire, which turns unrelated resets
             * over the device's life into a factory reset. Refuse to count. */
            ESP_LOGE(TAG, "no timer available, abandoning the reset chain");
            s_chain.count = 0;
            return;
        }
    }

    esp_timer_start_once(timer, (uint64_t)CONFIG_GREENHOUSE_FACTORY_RESET_WINDOW_S * 1000000);
}

#if CONFIG_GREENHOUSE_FACTORY_RESET_CLICK
/*
 * The only output this board has. A headless device that has just thrown away
 * the owner's settings should say so to someone standing next to it, and the
 * relay clicking three times is audible across a greenhouse.
 */
static void click_confirmation(void)
{
    for (int i = 0; i < 3; i++) {
        relay_set(true);
        vTaskDelay(pdMS_TO_TICKS(120));
        relay_set(false);
        vTaskDelay(pdMS_TO_TICKS(180));
    }
}
#endif

static void do_reset(void)
{
    s_triggered = true;

    ESP_LOGW(TAG, "%d resets in a row: restoring factory defaults",
             CONFIG_GREENHOUSE_FACTORY_RESET_PRESSES);

    esp_err_t err = config_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not write defaults: %s", esp_err_to_name(err));
        return;
    }

    /* Polarity is part of what was just reset, so re-drive the pin rather than
     * leaving it asserting the old sense until the first controller tick. */
    relay_refresh();

    ESP_LOGW(TAG, "access point restored with the built-in password");

#if CONFIG_GREENHOUSE_FACTORY_RESET_CLICK
    click_confirmation();
#endif
}

void factory_reset_check(void)
{
    esp_reset_reason_t reason = esp_reset_reason();

    /*
     * Only the reset button continues a chain. A power cut cannot: RTC memory
     * does not survive it, and the magic check catches that. A crash or a
     * watchdog reboot deliberately breaks the chain, so a device stuck in a
     * boot loop wipes its configuration rather than keeping the fault, which
     * would be a much worse failure than the loop itself.
     */
    if (reason != ESP_RST_EXT || s_chain.magic != FR_MAGIC) {
        s_chain.magic = FR_MAGIC;
        s_chain.count = 0;
        return;
    }

    s_chain.count++;

    if (s_chain.count >= CONFIG_GREENHOUSE_FACTORY_RESET_PRESSES) {
        s_chain.count = 0;
        do_reset();
        return;
    }

    ESP_LOGI(TAG, "reset %u of %d; %d more within %ds restores factory defaults",
             s_chain.count, CONFIG_GREENHOUSE_FACTORY_RESET_PRESSES,
             CONFIG_GREENHOUSE_FACTORY_RESET_PRESSES - (int)s_chain.count,
             CONFIG_GREENHOUSE_FACTORY_RESET_WINDOW_S);

    arm_window();
}

bool factory_reset_triggered(void)
{
    return s_triggered;
}
