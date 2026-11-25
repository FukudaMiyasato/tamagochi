#pragma once
#include <stdbool.h>

void pet_init(void);
// Descuenta el tiempo que pasó. Llamar seguido.
void pet_tick(void);
// Vida restante en milésimas (0..1000).
int pet_life(void);
bool pet_is_dead(void);
// Suma vida (en milésimas). Si estaba muerto, revive.
void pet_feed(int gain);
