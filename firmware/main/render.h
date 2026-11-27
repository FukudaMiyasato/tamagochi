// Dibujo de la escena: aro de vida + mascota. C puro, sin dependencias del
// ESP32, para poder previsualizarlo en la computadora.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SCREEN_W 240
#define SCREEN_H 240

// Expresiones de la mascota, en el orden de la hoja assets/mascota.webp.
typedef enum {
    SPR_SMILE,      // tranquila
    SPR_JOY,        // contenta
    SPR_SPARKLE,    // ojos brillantes
    SPR_WINK,       // guiño
    SPR_COOL,       // lentes de sol
    SPR_CRY,        // llorando
    SPR_ANGRY,      // molesta
    SPR_SURPRISED,  // "!"
    SPR_CONFUSED,   // "?"
    SPR_LOVE,       // corazones
    SPR_SLEEP,      // zzz
    SPR_HUNGRY,     // pensando en comida
    SPR_SICK,       // mareada, verde
    SPR_DIZZY,      // estrellas
    SPR_DEAD,       // fantasma
    SPR_COUNT,
} sprite_t;

#define SPRITE_W 176
#define SPRITE_H 150
#define SPRITES_BYTES (SPR_COUNT * SPRITE_W * SPRITE_H * 2)

#define SCENE_BARS 9
#define BAR_MAX_H 22

typedef struct {
    int16_t life;      // 0..1000, cuánto del aro está lleno
    uint8_t sprite;    // sprite_t
    int8_t bounce;     // desplazamiento vertical de la mascota, en píxeles
    int8_t sway;       // desplazamiento horizontal (se balancea al pensar)
    uint8_t ring_dim;  // el aro parpadea cuando queda poca vida
    int8_t dots;       // pensando: 0..2 = punto que salta; -1 = sin puntos
    uint8_t bars[SCENE_BARS];  // escuchando: alto de cada barra de volumen (0 = sin barras)
    uint8_t offline;   // "!" naranja abajo: sin Wi-Fi
} scene_t;

// Sprites generados por tools/make_sprites.py: RGB565 con los bytes en el
// orden de la pantalla, ya mezclados con el color de fondo.
void render_set_sprites(const uint16_t *data);

// Dibuja las filas [y0, y0+rows) en `out` (RGB565 en el orden de la pantalla).
void render_band(const scene_t *s, int y0, int rows, uint16_t *out);
