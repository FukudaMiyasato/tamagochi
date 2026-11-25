# tamagochi

Dos partes:

- **Web** (esta carpeta): dashboard que recibe audios por una API abierta, los transcribe con Whisper (OpenAI) y los lista en el home con audio, texto y fecha.
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
| `POST` | `/api/audios` | Recibe un audio, lo guarda y lo transcribe. Responde con el registro. |
| `GET` | `/api/audios` | Lista todos los audios, del más nuevo al más viejo. |
| `GET` | `/api/audios/:id` | Un registro. |
| `GET` | `/api/audios/:id/file` | El archivo de audio. |
| `POST` | `/api/audios/:id/transcribe` | Reintenta una transcripción que falló. |

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
  "audio_url": "/api/audios/1/file",
  "original_name": "nota.m4a",
  "mime": "audio/mp4",
  "size": 48213,
  "status": "done",
  "text": "Hola, esto es una prueba.",
  "error": null,
  "created_at": "2026-11-24T20:51:00.000Z"
}
```

`words` es la cantidad de palabras de la transcripción; el tamagochi la usa para saber cuánto comió.

Formatos: mp3, m4a, mp4, wav, webm, ogg, flac, aac. Máximo 25 MB en local y 4 MB en Vercel.

## Almacenamiento

- **En local**: los audios y la base SQLite se guardan en `data/` (fuera de git).
- **En Vercel** (no tiene disco persistente): los audios van a Vercel Blob y los registros a Postgres. En el proyecto de Vercel, pestaña Storage, conecta:
  1. Un store **Blob** con acceso público (crea `BLOB_READ_WRITE_TOKEN`).
  2. Una base **Postgres**, por ejemplo Neon (crea `DATABASE_URL`).
  3. En Settings → Environment Variables, agrega `OPENAI_API_KEY`.

  Después vuelve a desplegar. Mientras falte alguno, la API responde 503 indicando cuál.

La API no tiene autenticación: cualquiera que alcance el servidor puede enviar audios y leerlos.

# Firmware (tamagochi)

Placa: Spotpear Toy AI Core C3 1.28 (ESP32-C3, pantalla GC9A01 de 240x240, códec ES8311 con micrófono, botón BOOT).

- El aro alrededor de la pantalla es la vida. Se vacía en una hora; si llega a cero, el animalito muere.
- Para alimentarlo: mantén presionado el botón BOOT, habla y suelta. El audio se envía a la API.
- Cada palabra que se entiende devuelve 10% de vida (10 palabras llenan el aro). Si el audio llegó pero no se pudo transcribir, cuenta 3% por segundo.
- Un audio que sí alimenta también revive a un animalito muerto.
- La vida se guarda en la placa: si la apagas, al volver descuenta el tiempo que estuvo apagada.
- El Wi-Fi se toma del que ya tenía guardado el firmware original. Si no hay, crea `firmware/main/secrets.h` con `#define WIFI_SSID "..."` y `#define WIFI_PASSWORD "..."`.

Pines, URL de la API y reglas de vida están en [firmware/main/config.h](firmware/main/config.h).

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
