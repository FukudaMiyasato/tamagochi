#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_partition.h"

esp_err_t audio_init(void);
// Empieza a grabar del micrófono a la partición "rec".
void audio_start(void);
// Detiene la grabación y devuelve cuántos bytes de audio (PCM mono 16 bits) quedaron.
size_t audio_stop(void);
// false si la grabación se cortó sola al llegar al máximo.
bool audio_is_recording(void);
// Volumen actual, 0..32767. Sirve para animar la pantalla mientras escucha.
int audio_level(void);
// Deja la partición lista para la próxima grabación.
void audio_erase(size_t used_bytes);
const esp_partition_t *audio_partition(void);
