#pragma once
#include "esp_err.h"
#include "render.h"

esp_err_t display_init(void);
// Dibuja la escena completa en la pantalla.
void display_draw(const scene_t *scene);
