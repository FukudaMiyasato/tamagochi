const express = require('express');
const multer = require('multer');
const path = require('node:path');
const crypto = require('node:crypto');
const { getStore } = require('./store');

const PORT = process.env.PORT || 3000;
// Whisper acepta 25 MB, pero Vercel corta las peticiones en 4.5 MB.
const MAX_BYTES = process.env.VERCEL ? 4 * 1024 * 1024 : 25 * 1024 * 1024;

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

// Con silencio o ruido, Whisper suele inventar frases de cierre de video.
// Se descartan los tramos que él mismo marca como "sin voz" y esas frases típicas.
const NO_SPEECH_MAX = 0.5;
const HALLUCINATIONS = [
  /^(muchas )?gracias( por (ver|mirar|su atención|vernos))?( el video)?$/,
  /^thank(s| you)( for watching)?$/,
  /^subt[ií]tulos? (realizados?|creados?|hechos?) por/,
  /amara\.org/,
  /^(suscr[ií]bete|suscr[ií]banse)( al canal)?$/,
  /^(hasta la pr[oó]xima|nos vemos en el pr[oó]ximo video)$/,
];
const isHallucination = (text) => {
  const t = text.toLowerCase().replace(/[.,!¡¿?…"]/g, '').replace(/\s+/g, ' ').trim();
  return HALLUCINATIONS.some((re) => re.test(t));
};

async function transcribe(buffer, mime, filename) {
  const key = process.env.OPENAI_API_KEY;
  if (!key) throw new Error('Falta OPENAI_API_KEY');

  const form = new FormData();
  form.append('model', 'whisper-1');
  form.append('language', process.env.WHISPER_LANGUAGE || 'es');
  form.append('temperature', '0');
  form.append('response_format', 'verbose_json');
  form.append('file', new Blob([buffer], { type: mime }), filename);

  const res = await fetch('https://api.openai.com/v1/audio/transcriptions', {
    method: 'POST',
    headers: { Authorization: `Bearer ${key}` },
    body: form,
  });
  const body = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(body.error?.message || `OpenAI respondió ${res.status}`);

  const segments = Array.isArray(body.segments) ? body.segments : [{ text: body.text ?? '' }];
  return segments
    .filter((seg) => !(seg.no_speech_prob > NO_SPEECH_MAX))
    .map((seg) => String(seg.text || '').trim())
    .filter((text) => text && !isHallucination(text))
    .join(' ');
}

async function runTranscription(id, buffer) {
  const store = getStore();
  const row = await store.get(id);
  try {
    const text = await transcribe(buffer ?? (await store.readAudio(row)), row.mime, row.filename);
    await store.update(id, { status: 'done', text });
  } catch (err) {
    await store.update(id, { status: 'error', error: String(err.message || err) });
  }
  return publicRow(await store.get(id));
}

const countWords = (text) => (String(text || '').match(/[\p{L}\p{N}]+/gu) || []).length;

function publicRow(r) {
  return {
    id: r.id,
    audio_url: `/api/audios/${r.id}/file`,
    original_name: r.original_name,
    mime: r.mime,
    size: r.size,
    status: r.status,
    text: r.text,
    words: countWords(r.text), // el tamagochi se alimenta según esto
    error: r.error,
    created_at: r.created_at,
  };
}

const app = express();
const upload = multer({ storage: multer.memoryStorage(), limits: { fileSize: MAX_BYTES, files: 1 } });
const wrap = (fn) => (req, res, next) => fn(req, res, next).catch(next);

// API abierta: sin autenticación y con CORS para cualquier origen.
app.use('/api', (req, res, next) => {
  res.set('Access-Control-Allow-Origin', '*');
  res.set('Access-Control-Allow-Headers', 'Content-Type, Authorization');
  res.set('Access-Control-Allow-Methods', 'GET,POST,OPTIONS');
  if (req.method === 'OPTIONS') return res.sendStatus(204);
  next();
});

// Si existe API_KEY, solo quien la envíe puede subir audios:
//   Authorization: Bearer <clave>
// Leer el listado y escuchar los audios sigue siendo público (lo usa el home).
function requireKey(req, res, next) {
  const expected = process.env.API_KEY;
  if (!expected) return next();
  const given = (req.headers.authorization || '').replace(/^Bearer\s+/i, '');
  const a = crypto.createHash('sha256').update(given).digest();
  const b = crypto.createHash('sha256').update(expected).digest();
  if (!crypto.timingSafeEqual(a, b)) {
    return res.status(401).json({ error: 'Clave incorrecta o ausente' });
  }
  next();
}

// POST /api/audios
//   a) multipart/form-data con el archivo en el campo "audio" (o "file")
//   b) el audio crudo en el body con Content-Type: audio/*
app.post(
  '/api/audios',
  requireKey,
  upload.any(),
  express.raw({ type: ['audio/*', 'video/webm', 'video/mp4', 'application/octet-stream'], limit: MAX_BYTES }),
  wrap(async (req, res) => {
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
    const kind = detectAudio(mime, name);
    if (!kind) {
      return res.status(415).json({ error: 'Formato no soportado. Usa mp3, m4a, mp4, wav, webm, ogg, flac o aac.' });
    }
    const filename = `${Date.now()}-${crypto.randomBytes(6).toString('hex')}.${kind.ext}`;
    const id = await getStore().create({ buffer, filename, mime: kind.mime, originalName: name });
    res.status(201).json(await runTranscription(id, buffer));
  })
);

app.get('/api/audios', wrap(async (req, res) => {
  res.json((await getStore().list()).map(publicRow));
}));

app.get('/api/audios/:id', wrap(async (req, res) => {
  const row = await getStore().get(Number(req.params.id));
  if (!row) return res.status(404).json({ error: 'No existe' });
  res.json(publicRow(row));
}));

app.get('/api/audios/:id/file', wrap(async (req, res) => {
  const store = getStore();
  const row = await store.get(Number(req.params.id));
  if (!row) return res.status(404).json({ error: 'No existe' });
  store.sendAudio(row, res);
}));

// Reintenta la transcripción de un audio que falló.
app.post('/api/audios/:id/transcribe', wrap(async (req, res) => {
  const store = getStore();
  const id = Number(req.params.id);
  const row = await store.get(id);
  if (!row) return res.status(404).json({ error: 'No existe' });
  if (row.status === 'transcribing') return res.status(409).json({ error: 'Ya se está transcribiendo' });
  await store.update(id, { status: 'transcribing' });
  res.json(await runTranscription(id));
}));

app.use(express.static(path.join(__dirname, 'public')));

app.use((err, req, res, next) => {
  if (err?.code === 'LIMIT_FILE_SIZE' || err?.type === 'entity.too.large') {
    return res.status(413).json({ error: `El audio supera los ${Math.round(MAX_BYTES / 1048576)} MB` });
  }
  if (err?.status === 503) return res.status(503).json({ error: err.message });
  console.error(err);
  res.status(500).json({ error: 'Error interno' });
});

// En Vercel la app se exporta y la plataforma la sirve; en local escucha en un puerto.
if (!process.env.VERCEL) {
  app.listen(PORT, () => {
    console.log(`tamagochi escuchando en http://localhost:${PORT}`);
    if (!process.env.OPENAI_API_KEY) console.warn('Aviso: falta OPENAI_API_KEY; los audios se guardarán sin transcribir.');
  });
}

module.exports = app;
