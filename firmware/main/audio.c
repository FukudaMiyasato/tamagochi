#include "audio.h"

#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "es8311_codec.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "audio";

#define FRAMES 512  // 32 ms por bloque

static esp_codec_dev_handle_t s_dev;
static const esp_partition_t *s_part;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_stopped;
static volatile bool s_recording;
static volatile size_t s_bytes;
static volatile int s_level;

static esp_err_t codec_init(void)
{
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &i2c_bus), TAG, "i2c");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_chan_handle_t rx = NULL;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &rx), TAG, "i2s");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = PIN_I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s std");

    audio_codec_i2s_cfg_t i2s_if = { .port = I2S_NUM_0, .rx_handle = rx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_if);
    audio_codec_i2c_cfg_t i2c_if = { .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c_bus };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_if);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_FAIL, TAG, "interfaces del códec");

    // Solo captura. pa_pin = -1 deja el amplificador del parlante apagado.
    es8311_codec_cfg_t es_cfg = {
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .pa_pin = -1,
        .use_mclk = true,
        .digital_mic = false,
        .no_dac_ref = true,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "es8311");

    esp_codec_dev_cfg_t dev_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec_if, .data_if = data_if };
    s_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_dev, ESP_FAIL, TAG, "codec dev");

    // El códec entrega la misma señal del micrófono en los dos canales.
    esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 2, .sample_rate = SAMPLE_RATE };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_dev, &fs), TAG, "codec open");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(s_dev, MIC_GAIN_DB), TAG, "ganancia");
    return ESP_OK;
}

static void audio_task(void *arg)
{
    static int16_t stereo[FRAMES * 2];
    static int16_t mono[FRAMES];

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Descarta lo que quedó acumulado mientras nadie leía.
        for (int i = 0; i < 4; i++) {
            esp_codec_dev_read(s_dev, stereo, sizeof(stereo));
        }

        size_t written = 0;
        int max_peak = 0;
        while (s_recording && written + sizeof(mono) <= REC_MAX_BYTES) {
            if (esp_codec_dev_read(s_dev, stereo, sizeof(stereo)) != ESP_CODEC_DEV_OK) {
                ESP_LOGE(TAG, "falló la lectura del micrófono");
                break;
            }
            int peak = 0;
            for (int i = 0; i < FRAMES; i++) {
                mono[i] = stereo[i * 2];
                int a = abs(mono[i]);
                if (a > peak) {
                    peak = a;
                }
            }
            s_level = peak;
            if (peak > max_peak) {
                max_peak = peak;
            }
            if (esp_partition_write(s_part, written, mono, sizeof(mono)) != ESP_OK) {
                ESP_LOGE(TAG, "falló la escritura en flash");
                break;
            }
            written += sizeof(mono);
            s_bytes = written;
        }
        ESP_LOGI(TAG, "grabación terminada, pico máximo %d de 32767", max_peak);
        s_recording = false;
        s_level = 0;
        xSemaphoreGive(s_stopped);
    }
}

esp_err_t audio_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "rec");
    ESP_RETURN_ON_FALSE(s_part, ESP_ERR_NOT_FOUND, TAG, "no existe la partición rec");
    ESP_RETURN_ON_ERROR(codec_init(), TAG, "códec");

    // Limpia lo que haya quedado de una grabación interrumpida.
    static uint8_t page[4096];
    for (size_t off = 0; off < s_part->size; off += sizeof(page)) {
        esp_partition_read(s_part, off, page, sizeof(page));
        for (size_t i = 0; i < sizeof(page); i++) {
            if (page[i] != 0xFF) {
                esp_partition_erase_range(s_part, off, sizeof(page));
                break;
            }
        }
    }

    s_stopped = xSemaphoreCreateBinary();
    xTaskCreate(audio_task, "audio", 4096, NULL, 10, &s_task);
    return ESP_OK;
}

void audio_start(void)
{
    xSemaphoreTake(s_stopped, 0);
    s_bytes = 0;
    s_recording = true;
    xTaskNotifyGive(s_task);
}

size_t audio_stop(void)
{
    s_recording = false;
    xSemaphoreTake(s_stopped, pdMS_TO_TICKS(2000));
    return s_bytes;
}

bool audio_is_recording(void) { return s_recording; }
int audio_level(void) { return s_level; }
const esp_partition_t *audio_partition(void) { return s_part; }

void audio_erase(size_t used_bytes)
{
    size_t len = (used_bytes + 4095) / 4096 * 4096;
    if (len > s_part->size) {
        len = s_part->size;
    }
    if (len) {
        esp_partition_erase_range(s_part, 0, len);
    }
}
