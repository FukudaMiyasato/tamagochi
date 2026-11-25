const express = require('express');
const multer = require('multer');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { DatabaseSync } = require('node:sqlite');

const PORT = process.env.PORT || 3000;
const DATA_DIR = path.join(__dirname, 'data');
const AUDIO_DIR = path.join(DATA_DIR, 'audios');
const MAX_BYTES = 25 * 1024 * 1024; // límite de la API de Whisper

fs.mkdirSync(AUDIO_DIR, { recursive: true });

const db = new DatabaseSync(path.join(DATA_DIR, 'tamagochi.db'));
db.exec(`
  CREATE TABLE IF NOT EXISTS audios (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    filename      TEXT NOT NULL,
    original_name TEXT,
    mime          TEXT NOT NULL,
    size          INTEGER NOT NULL,
    status        TEXT NOT NULL,          -- transcribing | done | error
    text          TEXT,
    error         TEXT,
    created_at    TEXT NOT NULL
  )
`);
// Lo que quedó a medias si el servidor se reinició mientras transcribía.
db.prepare(
  "UPDATE audios SET status = 'error', error = 'El servidor se reinició durante la transcripción' WHERE status = 'transcribing'"
).run();

const MIME_BY_EXT = {
  mp3: 'audio/mpeg', mpga: 'audio/mpeg', mpeg: 'audio/mpeg', m4a: 'audio/mp4', mp4: 'audio/mp4',
  wav: 'audio/wav', webm: 'audio/webm', ogg: 'audio/ogg', oga: 'audio/ogg', opus: 'audio/ogg',
  flac: 'audio/flac', aac: 'audio/aac',
};
const EXT_BY_MIME = {
  'audio/mpeg': 'mp3', 'audio/mp3': 'mp3', 'audio/mp4': 'm4a', 'audio/x-m4a': 'm4a', 'audio/m4a': 'm4a',
  'audio/wav': 'wav', 'audio/x-wav': 'wav', 'audio/wave': 'wav', 'audio/webm': 'webm', 'video/webm': 'webm',
  'audio/ogg': 'ogg', 'audio/opus': 'ogg', 'audio/flac': 'flac', 'audio/x-flac': 'flac', 'audio/aac': 'aac',
  'video/mp4': 'mp4',
};

/** Devuelve { ext, mime } si el archivo parece audio, o null. */
function detectAudio(mime, name) {
  const cleanMime = String(mime || '').split(';')[0].trim().toLowerCase();
  const nameExt = path.extname(name || '').slice(1).toLowerCase();
  if (MIME_BY_EXT[nameExt]) return { ext: nameExt, mime: MIME_BY_EXT[nameExt] };
  if (EXT_BY_MIME[cleanMime]) {
    const ext = EXT_BY_MIME[cleanMime];
    return { ext, mime: MIME_BY_EXT[ext] };
  }
  return null;
}

async function transcribe(filePath, mime) {
  const key = process.env.OPENAI_API_KEY;
  if (!key) throw new Error('Falta OPENAI_API_KEY en el archivo .env');

  const form = new FormData();
  form.append('model', 'whisper-1');
  form.append('file', new Blob([fs.readFileSync(filePath)], { type: mime }), path.basename(filePath));

  const res = await fetch('https://api.openai.com/v1/audio/transcriptions', {
    method: 'POST',
    headers: { Authorization: `Bearer ${key}` },
    body: form,
  });
  const body = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(body.error?.message || `OpenAI respondió ${res.status}`);
  return body.text ?? '';
}

async function runTranscription(id) {
  const row = db.prepare('SELECT * FROM audios WHERE id = ?').get(id);
  try {
    const text = await transcribe(path.join(AUDIO_DIR, row.filename), row.mime);
    db.prepare("UPDATE audios SET status = 'done', text = ?, error = NULL WHERE id = ?").run(text, id);
  } catch (err) {
    db.prepare("UPDATE audios SET status = 'error', error = ? WHERE id = ?").run(String(err.message || err), id);
  }
  return publicRow(db.prepare('SELECT * FROM audios WHERE id = ?').get(id));
}

function publicRow(r) {
  return {
    id: r.id,
    audio_url: `/api/audios/${r.id}/file`,
    original_name: r.original_name,
    mime: r.mime,
    size: r.size,
    status: r.status,
    text: r.text,
    error: r.error,
    created_at: r.created_at,
  };
}

function saveAudio(buffer, mime, originalName) {
  const kind = detectAudio(mime, originalName);
  if (!kind) return null;
  const filename = `${Date.now()}-${crypto.randomBytes(6).toString('hex')}.${kind.ext}`;
  fs.writeFileSync(path.join(AUDIO_DIR, filename), buffer);
  const info = db
    .prepare(
      "INSERT INTO audios (filename, original_name, mime, size, status, created_at) VALUES (?, ?, ?, ?, 'transcribing', ?)"
    )
    .run(filename, originalName || null, kind.mime, buffer.length, new Date().toISOString());
  return Number(info.lastInsertRowid);
}

const app = express();
const upload = multer({ storage: multer.memoryStorage(), limits: { fileSize: MAX_BYTES, files: 1 } });

// API abierta: sin autenticación y con CORS para cualquier origen.
app.use('/api', (req, res, next) => {
  res.set('Access-Control-Allow-Origin', '*');
  res.set('Access-Control-Allow-Headers', 'Content-Type');
  res.set('Access-Control-Allow-Methods', 'GET,POST,OPTIONS');
  if (req.method === 'OPTIONS') return res.sendStatus(204);
  next();
});

// POST /api/audios
//   a) multipart/form-data con el archivo en el campo "audio" (o "file")
//   b) el audio crudo en el body con Content-Type: audio/*
app.post(
  '/api/audios',
  upload.any(),
  express.raw({ type: ['audio/*', 'video/webm', 'video/mp4', 'application/octet-stream'], limit: MAX_BYTES }),
  async (req, res) => {
    let buffer, mime, name;
    const file = (req.files || []).find((f) => f.fieldname === 'audio' || f.fieldname === 'file') || req.files?.[0];
    if (file) {
      ({ buffer, mimetype: mime, originalname: name } = file);
    } else if (Buffer.isBuffer(req.body) && req.body.length) {
      buffer = req.body;
      mime = req.headers['content-type'];
      name = req.query.filename;
    }
    if (!buffer?.length) {
      return res.status(400).json({ error: 'Envía un audio en el campo "audio" (multipart/form-data) o como body con Content-Type audio/*' });
    }
    const id = saveAudio(buffer, mime, name);
    if (!id) {
      return res.status(415).json({ error: 'Formato no soportado. Usa mp3, m4a, mp4, wav, webm, ogg, flac o aac.' });
    }
    res.status(201).json(await runTranscription(id));
  }
);

app.get('/api/audios', (req, res) => {
  res.json(db.prepare('SELECT * FROM audios ORDER BY id DESC').all().map(publicRow));
});

app.get('/api/audios/:id', (req, res) => {
  const row = db.prepare('SELECT * FROM audios WHERE id = ?').get(Number(req.params.id));
  if (!row) return res.status(404).json({ error: 'No existe' });
  res.json(publicRow(row));
});

app.get('/api/audios/:id/file', (req, res) => {
  const row = db.prepare('SELECT * FROM audios WHERE id = ?').get(Number(req.params.id));
  if (!row) return res.status(404).json({ error: 'No existe' });
  res.set('X-Content-Type-Options', 'nosniff');
  res.type(row.mime).sendFile(path.join(AUDIO_DIR, row.filename));
});

// Reintenta la transcripción de un audio que falló.
app.post('/api/audios/:id/transcribe', async (req, res) => {
  const id = Number(req.params.id);
  const row = db.prepare('SELECT * FROM audios WHERE id = ?').get(id);
  if (!row) return res.status(404).json({ error: 'No existe' });
  if (row.status === 'transcribing') return res.status(409).json({ error: 'Ya se está transcribiendo' });
  db.prepare("UPDATE audios SET status = 'transcribing', error = NULL WHERE id = ?").run(id);
  res.json(await runTranscription(id));
});

app.use(express.static(path.join(__dirname, 'public')));

app.use((err, req, res, next) => {
  if (err?.code === 'LIMIT_FILE_SIZE' || err?.type === 'entity.too.large') {
    return res.status(413).json({ error: 'El audio supera los 25 MB' });
  }
  console.error(err);
  res.status(500).json({ error: 'Error interno' });
});

app.listen(PORT, () => {
  console.log(`tamagochi escuchando en http://localhost:${PORT}`);
  if (!process.env.OPENAI_API_KEY) console.warn('Aviso: falta OPENAI_API_KEY; los audios se guardarán sin transcribir.');
});
