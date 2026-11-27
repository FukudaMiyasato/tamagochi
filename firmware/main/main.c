// Tamagochi: un animalito en el centro y un aro de vida que se vacía en una
// hora. Se alimenta con audios: mantén presionado el botón, habla y suelta.
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "config.h"
#include "display.h"
#include "led.h"
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
static uint8_t s_bars[SCENE_BARS];  // historial de volumen mientras escucha
static bool s_fail_no_voice;  // el último fallo fue por no oír voz

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

// Qué expresión muestra la mascota cuando no está pasando nada.
static sprite_t idle_sprite(int life, int64_t t)
{
    if (life < 100) {
        return SPR_SICK;
    }
    if (life < 300) {
        return SPR_CRY;
    }
    if (life < 600) {
        return SPR_HUNGRY;
    }
    if (life >= 900) {
        return (t % 6000) < 600 ? SPR_SPARKLE : SPR_COOL;
    }
    return (t % 6000) < 500 ? SPR_WINK : SPR_SMILE;
}

static void build_scene(scene_t *s, int shown_life)
{
    int64_t t = now_ms();
    bool dead = pet_is_dead();

    memset(s, 0, sizeof(*s));
    s->dots = -1;
    s->life = shown_life;
    s->offline = !net_connected();
    s->ring_dim = !dead && shown_life < 100 && (t / 400) % 2;

    switch (s_state) {
    case ST_LISTEN: {
        // Escuchando: barras que siguen el volumen y la mascota brinca con la voz.
        memcpy(s->bars, s_bars, sizeof(s->bars));
        s->sprite = SPR_SURPRISED;
        s->bounce = -(s_bars[SCENE_BARS - 1] / 4);
        break;
    }
    case ST_SEND: {
        // Pensando: se balancea de lado a lado y los puntos saltan en ola.
        static const int8_t SWAY[] = { 0, 2, 4, 5, 4, 2, 0, -2, -4, -5, -4, -2 };
        s->sprite = SPR_CONFUSED;
        s->sway = SWAY[(t / 100) % 12];
        s->dots = (t / 220) % 3;
        break;
    }
    case ST_FED:
        s->sprite = (t - s_state_since) < 1500 ? SPR_LOVE : SPR_JOY;
        s->bounce = (t / 200) % 2 ? -4 : 0;
        break;
    case ST_FAIL:
        s->sprite = s_fail_no_voice ? SPR_ANGRY : SPR_DIZZY;
        break;
    default:
        if (dead) {
            s->sprite = SPR_DEAD;
        } else {
            s->sprite = idle_sprite(shown_life, t);
            s->bounce = (t / (shown_life < 300 ? 900 : 500)) % 2 ? 3 : 0;
        }
        break;
    }
}

// Luz de la placa según lo que está pasando.
static void update_led(int life)
{
    int64_t t = now_ms();
    int wave = (int)(t % 2000);                       // respiración lenta: 0..2000
    int breath = 90 + (wave < 1000 ? wave : 2000 - wave) * 165 / 1000;  // 90..255

    switch (s_state) {
    case ST_LISTEN: {
        // Rojo mientras graba; más fuerte cuanto más alto se habla.
        int level = audio_level();
        int v = 80 + (level > 12000 ? 12000 : level) * 175 / 12000;
        led_set(v, 0, 0);
        return;
    }
    case ST_SEND:
        led_set(0, 0, (t / 150) % 2 ? 255 : 80);
        return;
    case ST_FED:
        led_set(255, 60, 140);
        return;
    case ST_FAIL:
        led_set(255, 90, 0);
        return;
    default:
        break;
    }
    if (pet_is_dead()) {
        led_set(0, 0, 0);
    } else if (life < 100) {
        led_set((t / 400) % 2 ? 255 : 0, 0, 0);
    } else if (life < 300) {
        led_set(breath, 0, 0);
    } else if (life < 600) {
        led_set(breath, breath * 2 / 3, 0);
    } else {
        led_set(0, breath, 0);
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
    led_init();
    pet_init();
    bool mic_ok = audio_init() == ESP_OK;
    if (!mic_ok) {
        ESP_LOGE(TAG, "micrófono no disponible");
    }
    net_start();
    audio_play(SOUND_HELLO);

    bool was_dead = pet_is_dead();
    int last_life = pet_life();
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
                    s_fail_no_voice = true;
                    audio_play(SOUND_NO_VOICE);
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
                bool revived = gain > 0 && pet_is_dead();
                pet_feed(gain);
                s_fail_no_voice = false;
                audio_play(gain <= 0 ? SOUND_FAIL : (revived ? SOUND_REVIVE : SOUND_FED));
                was_dead = pet_is_dead();
                last_life = pet_life();
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

        // Historial de volumen para las barras: entra un valor nuevo cada ~90 ms.
        static int64_t bar_at;
        static int bar_peak;
        if (s_state == ST_LISTEN) {
            int level = audio_level();
            if (level > bar_peak) {
                bar_peak = level;
            }
            if (now_ms() - bar_at >= 90) {
                bar_at = now_ms();
                memmove(s_bars, s_bars + 1, SCENE_BARS - 1);
                int h = 3 + bar_peak * (BAR_MAX_H - 3) / 9000;
                s_bars[SCENE_BARS - 1] = h > BAR_MAX_H ? BAR_MAX_H : h;
                bar_peak = 0;
            }
        } else {
            memset(s_bars, 0, sizeof(s_bars));
        }

        // El aro sube de a poco al comer, en vez de saltar.
        int life = pet_life();

        // Avisos sonoros: al morir y al cruzar los umbrales de hambre.
        if (s_state == ST_IDLE) {
            if (pet_is_dead() && !was_dead) {
                audio_play(SOUND_DEAD);
            } else if (!pet_is_dead() && ((last_life >= 300 && life < 300) || (last_life >= 100 && life < 100))) {
                audio_play(SOUND_HUNGRY);
            }
            was_dead = pet_is_dead();
            last_life = life;
        }
        update_led(shown_life);
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
