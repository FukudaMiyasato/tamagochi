#include "pet.h"

#include <time.h>

#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "pet";

#define SAVE_EVERY_MS (5 * 60 * 1000)
#define VALID_EPOCH 1700000000  // si el reloj marca menos, todavía no se sincronizó

static int64_t s_life_ms = LIFE_MAX_MS;
static bool s_dead;
static int64_t s_last_ms;
static int64_t s_last_save_ms;
static int64_t s_saved_epoch;      // hora del último guardado antes de este arranque
static bool s_offline_applied;     // ya se descontó el tiempo que estuvo apagado

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open("tama", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    time_t now = time(NULL);
    nvs_set_i32(h, "life_s", (int32_t)(s_life_ms / 1000));
    nvs_set_u8(h, "dead", s_dead);
    // Sin hora real no se sabrá cuánto estuvo apagado: mejor no descontar nada
    // que descontar de más con una hora vieja.
    nvs_set_i64(h, "epoch", (now > VALID_EPOCH && s_offline_applied) ? (int64_t)now : 0);
    nvs_commit(h);
    nvs_close(h);
    s_last_save_ms = now_ms();
}

static void die(void)
{
    s_life_ms = 0;
    s_dead = true;
    ESP_LOGW(TAG, "murió de hambre");
}

void pet_init(void)
{
    nvs_handle_t h;
    if (nvs_open("tama", NVS_READONLY, &h) == ESP_OK) {
        int32_t life_s;
        uint8_t dead = 0;
        if (nvs_get_i32(h, "life_s", &life_s) == ESP_OK) {
            s_life_ms = (int64_t)life_s * 1000;
        }
        nvs_get_u8(h, "dead", &dead);
        s_dead = dead;
        nvs_get_i64(h, "epoch", &s_saved_epoch);
        nvs_close(h);
    }
    if (s_saved_epoch == 0) {
        s_offline_applied = true;  // primer arranque: no hay nada que descontar
    }
    s_last_ms = s_last_save_ms = now_ms();
    ESP_LOGI(TAG, "vida %d/1000%s", pet_life(), s_dead ? " (muerto)" : "");
}

void pet_tick(void)
{
    int64_t t = now_ms();
    int64_t dt = t - s_last_ms;
    s_last_ms = t;

    // Al tener hora real, descuenta el tiempo que pasó apagado.
    if (!s_offline_applied && time(NULL) > VALID_EPOCH) {
        int64_t boot_epoch = (int64_t)time(NULL) - t / 1000;
        int64_t gap_s = boot_epoch - s_saved_epoch;
        s_offline_applied = true;
        if (gap_s > 0 && !s_dead) {
            ESP_LOGI(TAG, "estuvo apagado %lld s", gap_s);
            s_life_ms -= gap_s * 1000;
            if (s_life_ms <= 0) {
                die();
            }
        }
        save();
    }

    if (!s_dead) {
        s_life_ms -= dt;
        if (s_life_ms <= 0) {
            die();
            save();
        }
    }
    if (t - s_last_save_ms > SAVE_EVERY_MS) {
        save();
    }
}

int pet_life(void) { return (int)(s_life_ms * 1000 / LIFE_MAX_MS); }
bool pet_is_dead(void) { return s_dead; }

void pet_feed(int gain)
{
    if (gain <= 0) {
        return;
    }
    if (s_dead) {
        ESP_LOGI(TAG, "revive");
        s_dead = false;
        s_life_ms = 0;
    }
    s_life_ms += (int64_t)gain * LIFE_MAX_MS / 1000;
    if (s_life_ms > LIFE_MAX_MS) {
        s_life_ms = LIFE_MAX_MS;
    }
    ESP_LOGI(TAG, "comió +%d, vida %d/1000", gain, pet_life());
    save();
}
