#include "led.h"

#include "config.h"
#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "led";
static led_strip_handle_t s_strip;
static uint32_t s_last = 0xFFFFFFFF;

void led_init(void)
{
    led_strip_config_t strip = {
        .strip_gpio_num = PIN_LED,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = { .resolution_hz = 10 * 1000 * 1000 };
    if (led_strip_new_rmt_device(&strip, &rmt, &s_strip) != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo iniciar el LED");
        s_strip = NULL;
        return;
    }
    led_strip_clear(s_strip);
}

void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) {
        return;
    }
    r = r * LED_BRIGHTNESS / 255;
    g = g * LED_BRIGHTNESS / 255;
    b = b * LED_BRIGHTNESS / 255;
    uint32_t packed = (r << 16) | (g << 8) | b;
    if (packed == s_last) {
        return;  // sin cambios: no hace falta reenviar
    }
    s_last = packed;
    if (led_strip_set_pixel(s_strip, 0, r, g, b) == ESP_OK) {
        led_strip_refresh(s_strip);
    }
}
