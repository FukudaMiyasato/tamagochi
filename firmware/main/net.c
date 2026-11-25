#include "net.h"

#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs.h"

// Opcional: main/secrets.h con WIFI_SSID y WIFI_PASSWORD. Si existe, manda
// sobre el Wi-Fi que la placa tenga guardado.
#if __has_include("secrets.h")
#include "secrets.h"
#endif

static const char *TAG = "net";
static volatile bool s_connected;
static int s_last_reason;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        if (s_connected || s_last_reason != d->reason) {
            ESP_LOGW(TAG, "Wi-Fi desconectado (motivo %d), reintentando", d->reason);
        }
        s_last_reason = d->reason;
        s_connected = false;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        ESP_LOGI(TAG, "Wi-Fi conectado");
    }
}

// El firmware original (xiaozhi) guarda el Wi-Fi en NVS, espacio "wifi".
// Se reutiliza para no tener que escribir la clave en el código.
__attribute__((unused)) static bool load_stock_credentials(wifi_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t ssid_len = sizeof(cfg->sta.ssid), pass_len = sizeof(cfg->sta.password);
    bool ok = nvs_get_str(h, "ssid", (char *)cfg->sta.ssid, &ssid_len) == ESP_OK && cfg->sta.ssid[0];
    if (ok && nvs_get_str(h, "password", (char *)cfg->sta.password, &pass_len) != ESP_OK) {
        cfg->sta.password[0] = 0;
    }
    nvs_close(h);
    return ok;
}

void net_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    // Orden: secrets.h si existe, luego el Wi-Fi del firmware original, y por
    // último lo que el propio driver tenga guardado.
    wifi_config_t cfg = { 0 };
    const char *source;
#ifdef WIFI_SSID
    strlcpy((char *)cfg.sta.ssid, WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, WIFI_PASSWORD, sizeof(cfg.sta.password));
    source = "secrets.h";
#else
    source = "NVS del firmware original";
    if (!load_stock_credentials(&cfg)) {
        esp_wifi_get_config(WIFI_IF_STA, &cfg);
        source = "driver Wi-Fi";
    }
#endif
    if (!cfg.sta.ssid[0]) {
        ESP_LOGE(TAG, "no hay Wi-Fi configurado: crea main/secrets.h con WIFI_SSID y WIFI_PASSWORD");
        return;
    }
    ESP_LOGI(TAG, "Wi-Fi tomado de: %s", source);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_start();

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
}

bool net_connected(void) { return s_connected; }

static void put_le(uint8_t *p, uint32_t v, int n)
{
    for (int i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static void wav_header(uint8_t h[44], uint32_t pcm_bytes)
{
    memcpy(h, "RIFF", 4);
    put_le(h + 4, 36 + pcm_bytes, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4);              // tamaño del bloque fmt
    put_le(h + 20, 1, 2);               // PCM
    put_le(h + 22, 1, 2);               // mono
    put_le(h + 24, SAMPLE_RATE, 4);
    put_le(h + 28, SAMPLE_RATE * 2, 4); // bytes por segundo
    put_le(h + 32, 2, 2);               // bytes por muestra
    put_le(h + 34, 16, 2);              // bits por muestra
    memcpy(h + 36, "data", 4);
    put_le(h + 40, pcm_bytes, 4);
}

feed_result_t net_send_audio(const esp_partition_t *part, size_t pcm_bytes)
{
    feed_result_t r = { 0 };
    if (!s_connected) {
        ESP_LOGW(TAG, "sin Wi-Fi, no se envía");
        return r;
    }

    esp_http_client_config_t cfg = {
        .url = API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 60000,  // la transcripción puede tardar
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    const size_t chunk = 2048, body_max = 4096;
    uint8_t *buf = malloc(chunk);
    char *body = malloc(body_max);
    if (!http || !buf || !body) {
        goto done;
    }

    esp_http_client_set_header(http, "Content-Type", "audio/wav");
    if (esp_http_client_open(http, 44 + pcm_bytes) != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo conectar con la API");
        goto done;
    }

    uint8_t header[44];
    wav_header(header, pcm_bytes);
    if (esp_http_client_write(http, (const char *)header, sizeof(header)) < 0) {
        goto done;
    }
    for (size_t off = 0; off < pcm_bytes; off += chunk) {
        size_t n = pcm_bytes - off < chunk ? pcm_bytes - off : chunk;
        if (esp_partition_read(part, off, buf, n) != ESP_OK ||
            esp_http_client_write(http, (const char *)buf, n) < 0) {
            ESP_LOGE(TAG, "se cortó el envío a los %u bytes", (unsigned)off);
            goto done;
        }
    }

    esp_http_client_fetch_headers(http);
    r.http_status = esp_http_client_get_status_code(http);
    int len = esp_http_client_read_response(http, body, body_max - 1);
    body[len > 0 ? len : 0] = 0;
    ESP_LOGI(TAG, "API respondió %d: %.200s", r.http_status, body);

    r.delivered = r.http_status == 201;
    r.transcribed = r.delivered && strstr(body, "\"status\":\"done\"") != NULL;
    const char *w = strstr(body, "\"words\":");
    if (r.transcribed && w) {
        r.words = atoi(w + 8);
    }

done:
    if (http) {
        esp_http_client_cleanup(http);
    }
    free(buf);
    free(body);
    return r;
}
