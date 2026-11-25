// Placa: Spotpear Toy AI Core C3 1.28 (ESP32-C3, GC9A01 240x240, códec ES8311).
#pragma once

// A dónde se envía cada audio.
#define API_URL "https://tamagochi-omega.vercel.app/api/audios?filename=comida.wav"

// Pantalla GC9A01 (SPI). No tiene pin de reset ni de retroiluminación.
#define PIN_LCD_SCLK 0
#define PIN_LCD_MOSI 21
#define PIN_LCD_CS   20
#define PIN_LCD_DC   1

// Códec de audio ES8311 (I2C para control, I2S para el micrófono).
#define PIN_I2C_SDA  3
#define PIN_I2C_SCL  4
#define PIN_I2S_MCLK 10
#define PIN_I2S_BCLK 8
#define PIN_I2S_WS   6
#define PIN_I2S_DIN  7

// Botón BOOT: se mantiene presionado para grabar.
#define PIN_BUTTON   9

// Audio: 16 kHz, 16 bits, mono.
#define SAMPLE_RATE    16000
#define MIC_GAIN_DB    36.0f
#define REC_MAX_BYTES  (SAMPLE_RATE * 2 * 30)   // 30 segundos
#define REC_MIN_BYTES  (SAMPLE_RATE * 2 * 6 / 10) // menos de 0.6 s se descarta

// Vida: se vacía en una hora sin comer.
#define LIFE_MAX_MS        (60LL * 60 * 1000)
// Cada palabra que se entiende en el audio devuelve 10% de vida (10 palabras = lleno).
#define GAIN_PER_WORD      100   // en milésimas
// Si el audio llegó pero no se pudo transcribir, cuenta por duración: 3% por segundo.
#define GAIN_PER_SECOND    30
