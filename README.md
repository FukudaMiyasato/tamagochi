# tamagochi

Dos partes:

- **Web** (esta carpeta): dashboard que recibe audios por una API, los transcribe con Whisper (OpenAI), les asigna un sentimiento y una categoría, y muestra en el home los gráficos y los textos. El audio no se guarda.
- **Firmware** ([firmware/](firmware/)): un tamagochi para ESP32-C3 con pantalla redonda que se alimenta enviando audios a esa API.

# Web

## Uso

Requiere Node 22.5 o superior.

```bash
npm install
cp .env.example .env   # y pon tu OPENAI_API_KEY
npm start
```

Home: http://localhost:3000

## API

| Método | Ruta | Descripción |
| --- | --- | --- |
| `POST` | `/api/audios` | Recibe un audio, lo transcribe y lo clasifica. Responde con el registro. |
| `GET` | `/api/audios` | Lista todos los audios, del más nuevo al más viejo. |
| `GET` | `/api/audios/:id` | Un registro. |
| `POST` | `/api/audios/:id/analyze` | Clasifica un texto que quedó sin sentimiento. |
| `GET` | `/api/audios/:id/file` | El audio, solo en registros antiguos que lo conservan. |
| `POST` | `/api/audios/:id/transcribe` | Reintenta una transcripción (solo registros antiguos con audio). |

Enviar un audio (multipart, campo `audio`):

```bash
curl -X POST http://localhost:3000/api/audios -F "audio=@nota.m4a"
```

O el audio crudo en el body:

```bash
curl -X POST "http://localhost:3000/api/audios?filename=nota.m4a" -H "Content-Type: audio/mp4" --data-binary @nota.m4a
```

Respuesta:

```json
{
  "id": 1,
  "audio_url": null,
  "original_name": "nota.m4a",
  "mime": "audio/mp4",
  "size": 48213,
  "status": "done",
  "text": "Hola, esto es una prueba.",
  "sentiment": "neutro",
  "category": "saludo",
  "words": 5,
  "error": null,
  "created_at": "2026-11-24T20:51:00.000Z"
}
```

`sentiment` es `positivo`, `negativo` o `neutro`. `category` es una de: `queja`, `pedido`, `pregunta`, `agradecimiento`, `saludo`, `idea`, `comentario`, `otro`. Las asigna un modelo de OpenAI (`gpt-4o-mini`; se cambia con `OPENAI_CHAT_MODEL`).

`words` es la cantidad de palabras de la transcripción; el tamagochi la usa para saber cuánto comió.

Formatos: mp3, m4a, mp4, wav, webm, ogg, flac, aac. Máximo 25 MB en local y 4 MB en Vercel.

## Almacenamiento

Solo se guardan los registros (texto, sentimiento, categoría y fecha); el audio se descarta después de transcribirlo.

- **En local**: base SQLite en `data/` (fuera de git).
- **En Vercel**: Postgres. En el proyecto, pestaña Storage, conecta una base **Postgres**, por ejemplo Neon (crea `DATABASE_URL`), y en Settings → Environment Variables agrega `OPENAI_API_KEY`. Después vuelve a desplegar.

## Clave para subir audios

Si defines la variable `API_KEY` (en `.env` o en Vercel), `POST /api/audios` solo acepta peticiones con esa clave:

```bash
curl -X POST http://localhost:3000/api/audios -H "Authorization: Bearer TU_CLAVE" -F "audio=@nota.m4a"
```

El tamagochi la envía si está definida como `API_KEY` en `firmware/main/secrets.h`. Sin la variable, cualquiera puede subir audios.

Leer el listado es público siempre, porque lo usa el home.

# Firmware (tamagochi)

Placa: Spotpear Toy AI Core C3 1.28 (ESP32-C3, pantalla GC9A01 de 240x240, códec ES8311 con micrófono, botón BOOT).

- El aro alrededor de la pantalla es la vida. Se vacía en una hora; si llega a cero, el animalito muere.
- Para alimentarlo: mantén presionado el botón BOOT, habla y suelta. El audio se envía a la API.
- Cada palabra que se entiende devuelve 10% de vida (10 palabras llenan el aro). Si el audio llegó pero no se pudo transcribir, cuenta 3% por segundo.
- Un audio que sí alimenta también revive a un animalito muerto.
- La vida se guarda en la placa: si la apagas, al volver descuenta el tiempo que estuvo apagada.
- El Wi-Fi se toma del que ya tenía guardado el firmware original. Si no hay, crea `firmware/main/secrets.h` con `#define WIFI_SSID "..."` y `#define WIFI_PASSWORD "..."`.

- **Mascota:** las 15 expresiones salen de `assets/mascota.webp`. Para cambiarla, reemplaza esa imagen (misma disposición de 5x3) y ejecuta `python tools/make_sprites.py`, que regenera los sprites de la placa y de la web.
- **Sonidos:** melodías cortas por el parlante al encender, comer, fallar, tener hambre, morir y revivir. Volumen en `SOUND_VOLUME` (0 lo silencia).
- **Luz:** el LED de la placa respira en verde, amarillo o rojo según la vida; rojo fijo al grabar, azul al enviar, rosa al comer y naranja si falla. Brillo en `LED_BRIGHTNESS`.

Pines, URL de la API, volumen, brillo y reglas de vida están en [firmware/main/config.h](firmware/main/config.h).

Compilar y cargar (ESP-IDF 5.4):

```bash
. ~/esp/esp-idf/export.sh
cd firmware
idf.py set-target esp32c3
idf.py -p /dev/cu.usbmodem2101 flash monitor
```

Volver al firmware original (desde el respaldo en `backup/`, que no se sube a git):

```bash
esptool --port /dev/cu.usbmodem2101 write-flash 0 backup/firmware-original-16MB.bin
```
