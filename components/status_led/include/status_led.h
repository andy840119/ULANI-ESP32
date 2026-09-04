/*
 * On-board status LED.
 *
 * A single light that says what the board is doing without opening the web UI:
 *   - off      idle, nothing going on
 *   - solid    talking to a Tesserae server (fetching)
 *   - blinking streaming a frame to the calendar over BLE (uploading)
 *
 * Callers just flag when an activity starts and ends; this module owns the
 * pin, the precedence between activities (upload outranks fetch), and the
 * blink timer. Every call is a no-op when the LED is disabled in Kconfig, so
 * callers need no #ifdefs.
 *
 * The pin and polarity come from Kconfig board presets -- ESP-IDF has no board
 * database of its own, so a new board is a new preset there. Default is the
 * ESP32-C3 Super Mini's blue LED (GPIO8, active-low).
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STATUS_LED_FETCH  = 0, /* talking to Tesserae -> solid */
    STATUS_LED_UPLOAD = 1, /* streaming a frame over BLE -> blinking */
} status_led_activity_t;

/* Configures the pin and leaves the LED off. Call once at startup. */
esp_err_t status_led_init(void);

/*
 * Marks an activity as started (active = true) or finished (false). The LED
 * shows the highest-priority activity currently active: upload (blink) beats
 * fetch (solid) beats nothing (off). Safe to call from any task.
 */
void status_led_set(status_led_activity_t act, bool active);

#ifdef __cplusplus
}
#endif
