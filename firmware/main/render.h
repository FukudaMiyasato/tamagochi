// Dibujo de la escena: aro de vida + animalito. C puro, sin dependencias del
// ESP32, para poder previsualizarlo en la computadora.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SCREEN_W 240
#define SCREEN_H 240

typedef enum {
    FACE_IDLE,
    FACE_BLINK,
    FACE_SAD,
    FACE_LISTEN,
    FACE_CHEW_OPEN,
    FACE_CHEW_CLOSED,
    FACE_HAPPY,
    FACE_DEAD,
} face_t;

typedef enum { BODY_OK, BODY_HUNGRY, BODY_DEAD } body_t;

typedef struct {
    int16_t life;        // 0..1000, cuánto del aro está lleno
    uint8_t face;        // face_t
    uint8_t body;        // body_t
    int8_t bounce;       // desplazamiento vertical del animalito, en píxeles
    uint8_t ring_dim;    // el aro parpadea cuando queda poca vida
    uint8_t rec_radius;  // >0: punto rojo de "escuchando", con este radio
    int8_t dots;         // 0..2: punto encendido de "enviando"; -1 = ninguno
    uint8_t heart;       // corazón: comió
    uint8_t alert;       // "!" rojo arriba: no pudo comer
    uint8_t offline;     // "!" naranja abajo: sin Wi-Fi
    uint8_t halo;        // aureola: murió
} scene_t;

// Dibuja las filas [y0, y0+rows) en `out` (RGB565 con los bytes en el orden
// que espera la pantalla).
void render_band(const scene_t *s, int y0, int rows, uint16_t *out);
