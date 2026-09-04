#include "status_led.h"

#include "sdkconfig.h"

#if CONFIG_ULANI_STATUS_LED_ENABLE

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LED_GPIO  CONFIG_ULANI_STATUS_LED_GPIO
#define BLINK_MS  150

/* Kconfig bool: defined to 1 when active-low, absent (==0 to the pp) otherwise. */
#if CONFIG_ULANI_STATUS_LED_ACTIVE_LOW
#define LED_ON_LEVEL  0
#else
#define LED_ON_LEVEL  1
#endif

static const char *TAG = "status_led";

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_blink;
static uint8_t s_mask;    /* bit per active status_led_activity_t */
static bool    s_blink_on;

static void led_write(bool on)
{
    gpio_set_level(LED_GPIO, on ? LED_ON_LEVEL : !LED_ON_LEVEL);
}

static void blink_cb(void *arg)
{
    (void)arg;
    s_blink_on = !s_blink_on;
    led_write(s_blink_on);
}

/* Pick the light for the current activity set. Caller holds the lock. */
static void recompute(void)
{
    if (s_mask & (1u << STATUS_LED_UPLOAD)) {
        if (!esp_timer_is_active(s_blink)) {
            s_blink_on = true;
            led_write(true);
            esp_timer_start_periodic(s_blink, BLINK_MS * 1000);
        }
        return;
    }

    if (esp_timer_is_active(s_blink)) {
        esp_timer_stop(s_blink);
    }
    led_write((s_mask & (1u << STATUS_LED_FETCH)) != 0);
}

esp_err_t status_led_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }
    led_write(false);

    esp_timer_create_args_t args = { .callback = blink_cb, .name = "status_led" };
    err = esp_timer_create(&args, &s_blink);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "status LED on GPIO%d (active-%s)", LED_GPIO,
             LED_ON_LEVEL == 0 ? "low" : "high");
    return ESP_OK;
}

void status_led_set(status_led_activity_t act, bool active)
{
    if (!s_lock) {
        return; /* init failed or not called; fail quiet, the LED is cosmetic */
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (active) {
        s_mask |= (uint8_t)(1u << act);
    } else {
        s_mask &= (uint8_t)~(1u << act);
    }
    recompute();
    xSemaphoreGive(s_lock);
}

#else /* !CONFIG_ULANI_STATUS_LED_ENABLE */

esp_err_t status_led_init(void) { return ESP_OK; }
void status_led_set(status_led_activity_t act, bool active) { (void)act; (void)active; }

#endif
