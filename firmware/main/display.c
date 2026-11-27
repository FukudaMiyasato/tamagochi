#include "display.h"

#include "config.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "display";

// La pantalla se dibuja por franjas: no hay RAM para un cuadro completo (115 KB).
#define BAND_ROWS 24
#define BAND_BYTES (SCREEN_W * BAND_ROWS * 2)

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_band;
static SemaphoreHandle_t s_flushed;

// El envío por SPI es asíncrono: hay que esperar a que termine antes de
// reutilizar el buffer de la franja.
// Sprites de la mascota, incluidos en el firmware desde sprites.bin.
extern const uint8_t sprites_start[] asm("_binary_sprites_bin_start");

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flushed, &woken);
    return woken == pdTRUE;
}

esp_err_t display_init(void)
{
    render_set_sprites((const uint16_t *)sprites_start);
    s_flushed = xSemaphoreCreateBinary();
    s_band = heap_caps_malloc(BAND_BYTES, MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(s_flushed && s_band, ESP_ERR_NO_MEM, TAG, "sin memoria para la pantalla");

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BAND_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = GC9A01_PANEL_IO_SPI_CONFIG(PIN_LCD_CS, PIN_LCD_DC, on_color_done, NULL);
    io_cfg.pclk_hz = 40 * 1000 * 1000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io), TAG, "panel io");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,  // sin pin de reset: se reinicia por comando
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_gc9a01(io, &panel_cfg, &s_panel), TAG, "gc9a01");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), TAG, "mirror");

    // Registros de gamma propios de este panel, tomados del firmware de fábrica.
    const uint8_t reg62[] = { 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70 };
    const uint8_t reg63[] = { 0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70 };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x62, reg62, sizeof(reg62)), TAG, "0x62");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x63, reg63, sizeof(reg63)), TAG, "0x63");

    // Primer cuadro antes de encender, para no mostrar basura.
    scene_t blank = { .dots = -1 };
    display_draw(&blank);
    return esp_lcd_panel_disp_on_off(s_panel, true);
}

void display_draw(const scene_t *scene)
{
    for (int y = 0; y < SCREEN_H; y += BAND_ROWS) {
        render_band(scene, y, BAND_ROWS, s_band);
        if (esp_lcd_panel_draw_bitmap(s_panel, 0, y, SCREEN_W, y + BAND_ROWS, s_band) == ESP_OK) {
            xSemaphoreTake(s_flushed, pdMS_TO_TICKS(200));
        }
    }
}
