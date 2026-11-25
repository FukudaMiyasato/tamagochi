# tamagochi

Dashboard que recibe audios por una API abierta, los transcribe con Whisper (OpenAI) y los lista en el home con audio, texto y fecha.

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

Formatos: mp3, m4a, mp4, wav, webm, ogg, flac, aac. Máximo 25 MB.

Los audios y la base SQLite se guardan en `data/` (fuera de git).

La API no tiene autenticación: cualquiera que alcance el servidor puede enviar audios y leerlos.
