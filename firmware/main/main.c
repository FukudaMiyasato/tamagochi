// Tamagochi: un animalito en el centro y un aro de vida que se vacía en una
// hora. Se alimenta con audios: mantén presionado el botón, habla y suelta.
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "config.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "nvs_flash.h"
#include "pet.h"

static const char *TAG = "tama";

typedef enum { ST_IDLE, ST_LISTEN, ST_SEND, ST_FED, ST_FAIL } state_t;

static state_t s_state = ST_IDLE;
static int64_t s_state_since;
static size_t s_rec_bytes;
static volatile bool s_send_done;
static feed_result_t s_result;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void set_state(state_t st)
{
    s_state = st;
    s_state_since = now_ms();
}

// El envío corre aparte para que el animalito siga animado mientras tanto.
static void send_task(void *arg)
{
    s_result = net_send_audio(audio_partition(), s_rec_bytes);
    audio_erase(s_rec_bytes);
    s_send_done = true;
    vTaskDelete(NULL);
}

// Cuánta vida da un audio, en milésimas.
static int gain_for(const feed_result_t *r, size_t pcm_bytes)
{
    if (!r->delivered) {
        return 0;
    }
    int gain = r->transcribed ? r->words * GAIN_PER_WORD
                              : (int)(pcm_bytes / (SAMPLE_RATE * 2)) * GAIN_PER_SECOND;
    return gain > 1000 ? 1000 : gain;
}

static bool button_pressed(void)
{
    // Antirrebote: el estado cambia solo si se mantiene 30 ms.
    static bool stable;
    static int64_t changed_at;
    bool raw = gpio_get_level(PIN_BUTTON) == 0;
    if (raw == stable) {
        changed_at = now_ms();
    } else if (now_ms() - changed_at > 30) {
        stable = raw;
    }
    return stable;
}

static void build_scene(scene_t *s, int shown_life)
{
    int64_t t = now_ms();
    bool dead = pet_is_dead();
    bool hungry = shown_life < 300;

    memset(s, 0, sizeof(*s));
    s->dots = -1;
    s->life = shown_life;
    s->body = dead ? BODY_DEAD : (hungry ? BODY_HUNGRY : BODY_OK);
    s->offline = !net_connected();
    s->ring_dim = !dead && shown_life < 100 && (t / 400) % 2;

    switch (s_state) {
    case ST_LISTEN: {
        int level = audio_level();
        s->face = FACE_LISTEN;
        s->rec_radius = 7 + (level > 16000 ? 16000 : level) * 7 / 16000;
        break;
    }
    case ST_SEND:
        s->face = (t / 250) % 2 ? FACE_CHEW_OPEN : FACE_CHEW_CLOSED;
        s->dots = (t / 250) % 3;
        break;
    case ST_FED:
        s->face = FACE_HAPPY;
        s->heart = 1;
        s->bounce = (t / 200) % 2 ? -4 : 0;
        break;
    case ST_FAIL:
        s->face = dead ? FACE_DEAD : FACE_SAD;
        s->alert = 1;
        break;
    default:
        if (dead) {
            s->face = FACE_DEAD;
            s->halo = 1;
        } else if (hungry) {
            s->face = FACE_SAD;
            s->bounce = (t / 900) % 2 ? 2 : 0;
        } else {
            s->face = (t % 4000) < 160 ? FACE_BLINK : FACE_IDLE;
            s->bounce = (t / 500) % 2 ? 3 : 0;
        }
        break;
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);

    ESP_ERROR_CHECK(display_init());
    pet_init();
    bool mic_ok = audio_init() == ESP_OK;
    if (!mic_ok) {
        ESP_LOGE(TAG, "micrófono no disponible");
    }
    net_start();

    scene_t last = { 0 };
    bool first = true;
    int shown_life = pet_life();
    bool was_pressed = false;
    int test_ticks = 0;  // pulsación simulada por consola, para pruebas

    for (;;) {
        pet_tick();

        // Consola serie: 'r' simula mantener el botón 3 s; 's' imprime el estado.
        int ch = fgetc(stdin);
        if (ch == 'r') {
            test_ticks = 3000 / 30;
        } else if (ch == 's') {
            ESP_LOGI(TAG, "estado=%d vida=%d muerto=%d wifi=%d heap=%u", s_state, pet_life(), pet_is_dead(),
                     net_connected(), (unsigned)esp_get_free_heap_size());
        }
        if (test_ticks > 0) {
            test_ticks--;
        }
        bool pressed = button_pressed() || test_ticks > 0;

        switch (s_state) {
        case ST_IDLE:
            if (pressed && !was_pressed && mic_ok) {
                audio_start();
                set_state(ST_LISTEN);
                ESP_LOGI(TAG, "escuchando");
            }
            break;
        case ST_LISTEN:
            if (!pressed || !audio_is_recording()) {
                s_rec_bytes = audio_stop();
                ESP_LOGI(TAG, "grabó %u bytes (%.1f s)", (unsigned)s_rec_bytes, s_rec_bytes / (SAMPLE_RATE * 2.0f));
                if (s_rec_bytes < REC_MIN_BYTES) {
                    audio_erase(s_rec_bytes);  // toque accidental
                    set_state(ST_IDLE);
                } else if (audio_voiced_ms() < VOICE_MIN_MS) {
                    ESP_LOGI(TAG, "no se oyó voz: no se envía");
                    audio_erase(s_rec_bytes);
                    set_state(ST_FAIL);
                } else {
                    s_send_done = false;
                    xTaskCreate(send_task, "send", 8192, NULL, 5, NULL);
                    set_state(ST_SEND);
                }
            }
            break;
        case ST_SEND:
            if (s_send_done) {
                int gain = gain_for(&s_result, s_rec_bytes);
                ESP_LOGI(TAG, "entregado=%d transcrito=%d palabras=%d -> +%d", s_result.delivered,
                         s_result.transcribed, s_result.words, gain);
                pet_feed(gain);
                set_state(gain > 0 ? ST_FED : ST_FAIL);
            }
            break;
        case ST_FED:
        case ST_FAIL:
            if (now_ms() - s_state_since > 3000) {
                set_state(ST_IDLE);
            }
            break;
        }
        was_pressed = pressed;

        // El aro sube de a poco al comer, en vez de saltar.
        int life = pet_life();
        if (shown_life < life) {
            shown_life += (life - shown_life > 25) ? 25 : (life - shown_life);
        } else {
            shown_life = life;
        }

        scene_t scene;
        build_scene(&scene, shown_life);
        if (first || memcmp(&scene, &last, sizeof(scene)) != 0) {
            display_draw(&scene);
            last = scene;
            first = false;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
