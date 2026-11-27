#include "render.h"

#include <math.h>
#include <stdlib.h>

// RGB888 -> RGB565 con los bytes intercambiados (la pantalla los recibe big-endian).
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define C(r, g, b) ((uint16_t)((RGB565(r, g, b) >> 8) | ((RGB565(r, g, b) & 0xFF) << 8)))

#define COL_BG        C(0x0B, 0x10, 0x20)
#define COL_OFF       C(0x00, 0x00, 0x00)
#define COL_TRACK     C(0x1F, 0x29, 0x37)
#define COL_LIFE_HIGH C(0x4A, 0xDE, 0x80)
#define COL_LIFE_MID  C(0xFA, 0xCC, 0x15)
#define COL_LIFE_LOW  C(0xEF, 0x44, 0x44)
#define COL_LIFE_DIM  C(0x7F, 0x1D, 0x1D)
#define COL_DOT_ON    C(0xF9, 0xFA, 0xFB)
#define COL_DOT_OFF   C(0x37, 0x41, 0x51)
#define COL_OFFLINE   C(0xFB, 0x92, 0x3C)
#define COL_BAR       C(0xF8, 0x71, 0x71)

// Aro de vida.
#define CX 120
#define CY 120
#define R_SCREEN 120
#define R_OUT 117
#define R_IN 103

// Mascota: el centro del cuerpo queda cerca del centro de la pantalla.
#define SPR_X0 36
#define SPR_Y0 40

static const uint16_t *s_sprites;

void render_set_sprites(const uint16_t *data) { s_sprites = data; }

static inline bool in_circle(int x, int y, int cx, int cy, int r)
{
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

static inline bool in_rect(int x, int y, int x0, int y0, int x1, int y1)
{
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

void render_band(const scene_t *s, int y0, int rows, uint16_t *out)
{
    // Extremo del arco lleno, medido en sentido horario desde arriba.
    int life = s->life < 0 ? 0 : (s->life > 1000 ? 1000 : s->life);
    float theta = (float)life * (2.0f * (float)M_PI / 1000.0f);
    int ex = (int)lroundf(sinf(theta) * 1024.0f);
    int ey = (int)lroundf(-cosf(theta) * 1024.0f);
    bool past_half = life > 500;

    uint16_t life_color = life > 500 ? COL_LIFE_HIGH : (life > 200 ? COL_LIFE_MID : COL_LIFE_LOW);
    if (s->ring_dim) {
        life_color = COL_LIFE_DIM;
    }

    bool has_bars = false;
    for (int i = 0; i < SCENE_BARS; i++) {
        has_bars |= s->bars[i] != 0;
    }
    const uint16_t *sprite = s_sprites ? s_sprites + (s->sprite % SPR_COUNT) * SPRITE_W * SPRITE_H : 0;

    for (int y = y0; y < y0 + rows; y++) {
        int dy = y - CY;
        for (int x = 0; x < SCREEN_W; x++) {
            int dx = x - CX;
            int d2 = dx * dx + dy * dy;
            uint16_t c;

            if (d2 > R_SCREEN * R_SCREEN) {
                c = COL_OFF;  // fuera del círculo visible
            } else if (d2 >= R_IN * R_IN && d2 <= R_OUT * R_OUT) {
                bool filled;
                if (life >= 1000) {
                    filled = true;
                } else if (life <= 0) {
                    filled = false;
                } else {
                    bool right = dx > 0 || (dx == 0 && dy < 0);  // mitad derecha: 0..180 grados
                    bool before_end = (ey * dx - ex * dy) > 0;
                    filled = past_half ? (right || before_end) : (right && before_end);
                }
                c = filled ? life_color : COL_TRACK;
            } else if (d2 > R_OUT * R_OUT) {
                c = COL_OFF;
            } else {
                c = COL_BG;
                int px = x - SPR_X0 - s->sway, py = y - SPR_Y0 - s->bounce;
                if (s_sprites && px >= 0 && py >= 0 && px < SPRITE_W && py < SPRITE_H) {
                    c = sprite[py * SPRITE_W + px];
                }
                if (has_bars) {
                    // Barras de volumen centradas en y = 203, redondeadas arriba y abajo.
                    int bx = x - (CX - (SCENE_BARS * 8 - 4) / 2);
                    if (bx >= 0 && bx < SCENE_BARS * 8 && bx % 8 < 4) {
                        int half = (s->bars[bx / 8] + 1) / 2;
                        if (y >= 203 - half && y <= 203 + half) {
                            c = COL_BAR;
                        }
                    }
                } else if (s->dots >= 0) {
                    // Tres puntos; el activo salta.
                    for (int i = 0; i < 3; i++) {
                        bool on = i == s->dots;
                        if (in_circle(x, y, CX - 20 + i * 20, on ? 198 : 204, 5)) {
                            c = on ? COL_DOT_ON : COL_DOT_OFF;
                        }
                    }
                } else if (s->offline &&
                           (in_rect(x, y, CX - 2, 194, CX + 2, 205) || in_rect(x, y, CX - 2, 209, CX + 2, 213))) {
                    c = COL_OFFLINE;
                }
            }
            *out++ = c;
        }
    }
}
