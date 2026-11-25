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
#define COL_REC       C(0xEF, 0x44, 0x44)
#define COL_DOT_ON    C(0xF9, 0xFA, 0xFB)
#define COL_DOT_OFF   C(0x37, 0x41, 0x51)
#define COL_HEART     C(0xFB, 0x71, 0x85)
#define COL_ALERT     C(0xEF, 0x44, 0x44)
#define COL_OFFLINE   C(0xFB, 0x92, 0x3C)
#define COL_HALO      C(0xFD, 0xE0, 0x47)
#define COL_WHITE     C(0xFF, 0xFF, 0xFF)
#define COL_PUPIL     C(0x11, 0x18, 0x27)
#define COL_CHEEK     C(0xFB, 0x71, 0x85)
#define COL_MOUTH     C(0x9F, 0x12, 0x39)
#define COL_TEAR      C(0x60, 0xA5, 0xFA)

// Colores del cuerpo según el estado: contorno, relleno, brillo.
static const uint16_t BODY_COLORS[3][3] = {
    [BODY_OK]     = { C(0x13, 0x4E, 0x4A), C(0x5E, 0xEA, 0xD4), C(0xCC, 0xFB, 0xF1) },
    [BODY_HUNGRY] = { C(0x1E, 0x3A, 0x5F), C(0x93, 0xB4, 0xD8), C(0xDB, 0xE7, 0xF5) },
    [BODY_DEAD]   = { C(0x37, 0x41, 0x51), C(0x8B, 0x92, 0x9E), C(0xB6, 0xBC, 0xC6) },
};

// Aro de vida.
#define CX 120
#define CY 120
#define R_SCREEN 120
#define R_OUT 117
#define R_IN 103

// Animalito: cuadrícula de 16x16 ampliada x7, centrada.
#define GRID 16
#define SCALE 7
#define SPR_X0 (CX - GRID * SCALE / 2)
#define SPR_Y0 (CY + 2 - GRID * SCALE / 2)

// K contorno, B cuerpo, L brillo, '.' transparente.
static const char BODY[GRID][GRID + 1] = {
    "................",
    "...KK......KK...",
    "..KBBK....KBBK..",
    "..KBBBKKKKBBBK..",
    ".KBBBBBBBBBBBBK.",
    ".KBLBBBBBBBBBBK.",
    "KBBLBBBBBBBBBBBK",
    "KBBBBBBBBBBBBBBK",
    "KBBBBBBBBBBBBBBK",
    "KBBBBBBBBBBBBBBK",
    "KBBBBBBBBBBBBBBK",
    "KBBBBBBBBBBBBBBK",
    ".KBBBBBBBBBBBBK.",
    ".KBBBBBBBBBBBBK.",
    "..KKBBKKKKBBKK..",
    "...KKK....KKK...",
};

// Caras: filas 5 a 12 de la cuadrícula. W blanco, E pupila, K línea,
// M boca abierta, P mejilla, T lágrima.
#define FACE_ROW0 5
#define FACE_ROWS 8
static const char FACES[][FACE_ROWS][GRID + 1] = {
    [FACE_IDLE] = {
        "................",
        "................",
        "....WE....WE....",
        "....EE....EE....",
        "................",
        ".......KK.......",
        "................",
        "................",
    },
    [FACE_BLINK] = {
        "................",
        "................",
        "................",
        "....KK....KK....",
        "................",
        ".......KK.......",
        "................",
        "................",
    },
    [FACE_SAD] = {
        "................",
        "................",
        "................",
        "....WE....WE....",
        "....EE....EE....",
        ".....T..........",
        ".......KK.......",
        "......K..K......",
    },
    [FACE_LISTEN] = {
        "................",
        "................",
        "....WE....WE....",
        "....EE....EE....",
        "................",
        ".......MM.......",
        ".......MM.......",
        "................",
    },
    [FACE_CHEW_OPEN] = {
        "................",
        "................",
        "....K......K....",
        "...K.K....K.K...",
        "................",
        ".......MM.......",
        ".......MM.......",
        "................",
    },
    [FACE_CHEW_CLOSED] = {
        "................",
        "................",
        "....K......K....",
        "...K.K....K.K...",
        "................",
        "......KKKK......",
        "................",
        "................",
    },
    [FACE_HAPPY] = {
        "................",
        "................",
        "....K......K....",
        "...K.K....K.K...",
        ".PP..........PP.",
        "......K..K......",
        ".......KK.......",
        "................",
    },
    [FACE_DEAD] = {
        "................",
        "................",
        "...K.K....K.K...",
        "....K......K....",
        "...K.K....K.K...",
        "................",
        "......KKKK......",
        "................",
    },
};

static const char HEART[6][8] = {
    ".XX.XX.",
    "XXXXXXX",
    "XXXXXXX",
    ".XXXXX.",
    "..XXX..",
    "...X...",
};
#define HEART_SCALE 4
#define HEART_X0 (CX - 7 * HEART_SCALE / 2)
#define HEART_Y0 28

static inline bool in_circle(int x, int y, int cx, int cy, int r)
{
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

static inline bool in_rect(int x, int y, int x0, int y0, int x1, int y1)
{
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

// Devuelve true y el color si el animalito cubre este píxel.
static bool sprite_pixel(const scene_t *s, int x, int y, uint16_t *color)
{
    int px = x - SPR_X0;
    int py = y - SPR_Y0 - s->bounce;
    if (px < 0 || py < 0 || px >= GRID * SCALE || py >= GRID * SCALE) {
        return false;
    }
    int gx = px / SCALE, gy = py / SCALE;
    const uint16_t *pal = BODY_COLORS[s->body];

    if (gy >= FACE_ROW0 && gy < FACE_ROW0 + FACE_ROWS) {
        switch (FACES[s->face][gy - FACE_ROW0][gx]) {
        case 'W': *color = COL_WHITE; return true;
        case 'E': *color = COL_PUPIL; return true;
        case 'K': *color = COL_PUPIL; return true;
        case 'M': *color = COL_MOUTH; return true;
        case 'P': *color = COL_CHEEK; return true;
        case 'T': *color = COL_TEAR; return true;
        default: break;
        }
    }
    switch (BODY[gy][gx]) {
    case 'K': *color = pal[0]; return true;
    case 'B': *color = pal[1]; return true;
    case 'L': *color = pal[2]; return true;
    default: return false;
    }
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
                if (s->rec_radius && in_circle(x, y, CX, 42, s->rec_radius)) {
                    c = COL_REC;
                }
                if (s->heart) {
                    int hx = x - HEART_X0, hy = y - HEART_Y0;
                    if (hx >= 0 && hy >= 0 && hx < 7 * HEART_SCALE && hy < 6 * HEART_SCALE &&
                        HEART[hy / HEART_SCALE][hx / HEART_SCALE] == 'X') {
                        c = COL_HEART;
                    }
                }
                if (s->alert && (in_rect(x, y, CX - 3, 24, CX + 3, 44) || in_rect(x, y, CX - 3, 49, CX + 3, 55))) {
                    c = COL_ALERT;
                }
                if (s->halo) {
                    // Elipse hueca sobre la cabeza.
                    int hx = x - CX, hy = y - 46;
                    int v = hx * hx * 81 + hy * hy * 900;  // a = 30, b = 9
                    if (v <= 900 * 81 && v >= 900 * 81 * 45 / 100) {
                        c = COL_HALO;
                    }
                }
                if (s->dots >= 0) {
                    for (int i = 0; i < 3; i++) {
                        if (in_circle(x, y, CX - 20 + i * 20, 200, 5)) {
                            c = (i == s->dots) ? COL_DOT_ON : COL_DOT_OFF;
                        }
                    }
                } else if (s->offline &&
                           (in_rect(x, y, CX - 2, 190, CX + 2, 202) || in_rect(x, y, CX - 2, 206, CX + 2, 210))) {
                    c = COL_OFFLINE;
                }
                uint16_t sc;
                if (sprite_pixel(s, x, y, &sc)) {
                    c = sc;
                }
            }
            *out++ = c;
        }
    }
}
