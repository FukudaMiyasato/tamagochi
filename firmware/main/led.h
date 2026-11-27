#pragma once
#include <stdint.h>

void led_init(void);
// Color del LED de la placa; cada componente 0..255 antes de aplicar el brillo.
void led_set(uint8_t r, uint8_t g, uint8_t b);
