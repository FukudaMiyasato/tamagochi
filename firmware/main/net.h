#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_partition.h"

typedef struct {
    bool delivered;    // el servidor recibió y guardó el audio
    bool transcribed;  // además lo pudo transcribir
    int words;         // palabras que entendió
    int http_status;
} feed_result_t;

// Conecta al Wi-Fi (y sincroniza la hora) en segundo plano.
void net_start(void);
bool net_connected(void);
// Envía a la API el audio guardado en la partición, como WAV.
feed_result_t net_send_audio(const esp_partition_t *part, size_t pcm_bytes);
