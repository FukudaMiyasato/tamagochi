// Dos formas de guardar los registros, con la misma interfaz:
//   - cloud: Postgres. Se usa cuando existe DATABASE_URL (o POSTGRES_URL).
//   - local: SQLite en data/. Para desarrollo en tu máquina.
// El audio ya no se guarda: solo el texto, el sentimiento y la categoría.
// Los registros antiguos conservan su audio (en Vercel Blob o en data/audios).
const path = require('node:path');

const PG_URL = process.env.DATABASE_URL || process.env.POSTGRES_URL;

function createCloudStore() {
  const { Pool } = require('pg');
  const pool = new Pool({ connectionString: PG_URL, max: 2 });
  let ready;
  const init = () =>
    (ready ??= pool.query(`
      CREATE TABLE IF NOT EXISTS audios (
        id            SERIAL PRIMARY KEY,
        filename      TEXT NOT NULL,
        blob_url      TEXT NOT NULL,
        original_name TEXT,
        mime          TEXT NOT NULL,
        size          INTEGER NOT NULL,
        status        TEXT NOT NULL,
        text          TEXT,
        error         TEXT,
        created_at    TEXT NOT NULL
      );
      ALTER TABLE audios ALTER COLUMN blob_url DROP NOT NULL;
      ALTER TABLE audios ADD COLUMN IF NOT EXISTS sentiment TEXT;
      ALTER TABLE audios ADD COLUMN IF NOT EXISTS category TEXT;
    `));
  const q = async (text, params) => {
    await init();
    return (await pool.query(text, params)).rows;
  };

  return {
    kind: 'cloud',
    async create({ buffer, filename, mime, originalName }) {
      const rows = await q(
        `INSERT INTO audios (filename, original_name, mime, size, status, created_at)
         VALUES ($1, $2, $3, $4, 'transcribing', $5) RETURNING id`,
        [filename, originalName || null, mime, buffer.length, new Date().toISOString()]
      );
      return rows[0].id;
    },
    list: () => q('SELECT * FROM audios ORDER BY id DESC'),
    get: async (id) => (await q('SELECT * FROM audios WHERE id = $1', [id]))[0],
    update: (id, { status, text = null, error = null, sentiment = null, category = null }) =>
      q(
        `UPDATE audios SET status = $2, text = COALESCE($3, text), error = $4,
           sentiment = COALESCE($5, sentiment), category = COALESCE($6, category) WHERE id = $1`,
        [id, status, text, error, sentiment, category]
      ),
    hasAudio: (row) => Boolean(row.blob_url),
    async readAudio(row) {
      const res = await fetch(row.blob_url);
      if (!res.ok) throw new Error(`No se pudo leer el audio guardado (${res.status})`);
      return Buffer.from(await res.arrayBuffer());
    },
    sendAudio: (row, res) => res.redirect(302, row.blob_url),
  };
}

function createLocalStore() {
  const fs = require('node:fs');
  const { DatabaseSync } = require('node:sqlite');
  const dataDir = path.join(__dirname, 'data');
  const audioDir = path.join(dataDir, 'audios');
  fs.mkdirSync(audioDir, { recursive: true });

  const db = new DatabaseSync(path.join(dataDir, 'tamagochi.db'));
  db.exec(`
    CREATE TABLE IF NOT EXISTS audios (
      id            INTEGER PRIMARY KEY AUTOINCREMENT,
      filename      TEXT NOT NULL,
      original_name TEXT,
      mime          TEXT NOT NULL,
      size          INTEGER NOT NULL,
      status        TEXT NOT NULL,
      text          TEXT,
      error         TEXT,
      created_at    TEXT NOT NULL
    )
  `);
  const cols = db.prepare('PRAGMA table_info(audios)').all().map((c) => c.name);
  for (const col of ['sentiment', 'category']) {
    if (!cols.includes(col)) db.exec(`ALTER TABLE audios ADD COLUMN ${col} TEXT`);
  }
  // Lo que quedó a medias si el servidor se reinició mientras transcribía.
  db.prepare(
    "UPDATE audios SET status = 'error', error = 'El servidor se reinició durante la transcripción' WHERE status = 'transcribing'"
  ).run();

  return {
    kind: 'local',
    async create({ buffer, filename, mime, originalName }) {
      const info = db
        .prepare(
          "INSERT INTO audios (filename, original_name, mime, size, status, created_at) VALUES (?, ?, ?, ?, 'transcribing', ?)"
        )
        .run(filename, originalName || null, mime, buffer.length, new Date().toISOString());
      return Number(info.lastInsertRowid);
    },
    list: async () => db.prepare('SELECT * FROM audios ORDER BY id DESC').all(),
    get: async (id) => db.prepare('SELECT * FROM audios WHERE id = ?').get(id),
    update: async (id, { status, text = null, error = null, sentiment = null, category = null }) => {
      db.prepare(
        `UPDATE audios SET status = ?, text = COALESCE(?, text), error = ?,
           sentiment = COALESCE(?, sentiment), category = COALESCE(?, category) WHERE id = ?`
      ).run(status, text, error, sentiment, category, id);
    },
    hasAudio: (row) => fs.existsSync(path.join(audioDir, row.filename)),
    readAudio: async (row) => fs.readFileSync(path.join(audioDir, row.filename)),
    sendAudio: (row, res) => {
      res.set('X-Content-Type-Options', 'nosniff');
      res.type(row.mime).sendFile(path.join(audioDir, row.filename));
    },
  };
}

let store;
/** Devuelve el almacenamiento, o lanza un error explicando qué falta configurar. */
function getStore() {
  if (store) return store;
  if (PG_URL) return (store = createCloudStore());
  if (process.env.VERCEL) {
    const err = new Error('Falta conectar al proyecto en Vercel: Postgres (DATABASE_URL)');
    err.status = 503;
    throw err;
  }
  return (store = createLocalStore());
}

module.exports = { getStore };
