const express = require('express');
const path = require('path');
const fs = require('fs');

const app = express();
const PORT = process.env.PORT || 3000;

// ---------------------------------------------------------------------
// DELAY CONFIG (persisted to disk so it survives a server restart)
// ---------------------------------------------------------------------
const CONFIG_PATH = path.join(__dirname, 'config.json');

const MIN_INTERVAL_MS = 1000;     // don't allow silly-fast spam
const MAX_INTERVAL_MS = 300000;   // 5 min ceiling
const DEFAULT_CONFIG = { intervalMs: 5000 };

function loadConfig() {
  try {
    const raw = fs.readFileSync(CONFIG_PATH, 'utf8');
    const parsed = JSON.parse(raw);
    if (
      typeof parsed.intervalMs === 'number' &&
      parsed.intervalMs >= MIN_INTERVAL_MS &&
      parsed.intervalMs <= MAX_INTERVAL_MS
    ) {
      return { intervalMs: parsed.intervalMs };
    }
  } catch (err) {
    // File missing or corrupt on first run — fall through to default.
  }
  return { ...DEFAULT_CONFIG };
}

function saveConfig(cfg) {
  // Write to a temp file then rename, so a crash mid-write can't corrupt
  // config.json and take the whole endpoint down with it.
  const tmpPath = CONFIG_PATH + '.tmp';
  fs.writeFileSync(tmpPath, JSON.stringify(cfg, null, 2));
  fs.renameSync(tmpPath, CONFIG_PATH);
}

let config = loadConfig();
if (!fs.existsSync(CONFIG_PATH)) {
  try { saveConfig(config); } catch (err) { console.error('Could not write initial config.json:', err); }
}

// ---------------------------------------------------------------------
// IN-MEMORY STORES (reset if the server restarts — on Render's free tier
// that also happens on every redeploy and after the service sleeps)
// ---------------------------------------------------------------------
let history = [];
const MAX_HISTORY = 1000;

// Photos are kept in RAM as Buffers. Render's free instance has 512 MB, so
// cap both the count and the total size to stay well clear of that.
let photos = [];
const MAX_PHOTOS = 60;
const MAX_PHOTO_BYTES_TOTAL = 40 * 1024 * 1024;   // 40 MB
const MAX_PHOTO_BYTES = 2 * 1024 * 1024;          // single upload limit

// Monotonic counter so clients can ask "anything newer than N?"
let seq = 0;

// Changes whenever the data is wiped (reset button or server restart), so
// every open browser/phone knows to clear its map and start over.
let generation = Date.now();

function addPoint(point) {
  point.seq = ++seq;
  history.push(point);
  if (history.length > MAX_HISTORY) history.shift();
  return point;
}

function addPhoto(photo) {
  photo.seq = ++seq;
  photos.push(photo);
  let total = photos.reduce((sum, p) => sum + p.size, 0);
  while (photos.length > MAX_PHOTOS || total > MAX_PHOTO_BYTES_TOTAL) {
    total -= photos.shift().size;
  }
  return photo;
}

// Photo metadata without the image bytes, safe to send as JSON.
function photoMeta(p) {
  if (!p) return null;
  const { data, ...meta } = p;
  return { ...meta, url: `/api/photos/${p.id}.jpg` };
}

function parseCoord(value) {
  const n = parseFloat(value);
  return Number.isFinite(n) ? n : NaN;
}

// Serve the map frontend (public/index.html, etc.)
app.use(express.static(path.join(__dirname, 'public')));

app.get('/healthz', (req, res) => res.json({ status: 'ok' }));

// ---------------------------------------------------------------------
// PHOTO UPLOAD (Waveshare ESP32-S3 SIM7670G camera board)
// ---------------------------------------------------------------------
// The board POSTs the raw JPEG bytes as the request body, and puts the GPS
// fix in the query string, e.g.
//   POST /api/photo?trackerId=CAM-01&lat=23.05&lon=72.52&alt=15&ts=2026-10-19%2010:00:00
// Registered BEFORE the text body parser below so the binary body isn't
// mangled into a string.
app.post(
  '/api/photo',
  express.raw({ type: () => true, limit: MAX_PHOTO_BYTES }),
  (req, res) => {
    const body = req.body;

    if (!Buffer.isBuffer(body) || body.length < 4) {
      return res.status(400).json({ status: 'error', message: 'empty body — send raw JPEG bytes' });
    }
    // Every JPEG starts with FF D8.
    if (body[0] !== 0xff || body[1] !== 0xd8) {
      return res.status(400).json({ status: 'error', message: 'body is not a JPEG image' });
    }

    const q = req.query;
    const receivedAt = new Date().toISOString();
    const trackerId = String(q.trackerId || req.get('X-Tracker-Id') || 'CAM-01');
    const lat = parseCoord(q.lat);
    const lon = parseCoord(q.lon);
    const hasFix = !Number.isNaN(lat) && !Number.isNaN(lon) && !(lat === 0 && lon === 0);

    const photo = addPhoto({
      id: `${Date.now()}-${seq + 1}`,
      trackerId,
      lat: hasFix ? lat : null,
      lon: hasFix ? lon : null,
      altitude: q.alt ? `${q.alt}m` : null,
      timestamp: q.ts || null,
      receivedAt,
      size: body.length,
      data: body,
    });

    // A photo with a GPS fix also counts as a position report, so the
    // camera board shows up on the map trail just like the EC200U tracker.
    if (hasFix) {
      addPoint({
        trackerId,
        lat,
        lon,
        altitude: photo.altitude,
        timestamp: photo.timestamp,
        maplink: `https://maps.google.com/?q=${lat},${lon}`,
        receivedAt,
        photoId: photo.id,
      });
    }

    console.log(
      `[${receivedAt}] ${trackerId} photo ${Math.round(body.length / 1024)} KB` +
      (hasFix ? ` @ ${lat}, ${lon}` : ' (no GPS fix)')
    );

    res.status(200).json({ status: 'ok', intervalMs: config.intervalMs, photoId: photo.id });
  }
);

// The EC200U modem does not reliably send a proper "Content-Type: application/json"
// header (AT+QHTTPCFG="contenttype" on this sketch is set generically), so instead
// of express.json() we accept the raw body as text for ANY content type and parse
// it ourselves. This makes the server tolerant of whatever the modem actually sends.
app.use(express.text({ type: '*/*', limit: '1mb' }));

// This is the endpoint your sketch already posts to: SERVER_URL = "http://<ip>:3000/"
app.post('/', (req, res) => {
  let data;

  try {
    data = typeof req.body === 'string' ? JSON.parse(req.body) : req.body;
  } catch (err) {
    console.error('Could not parse incoming body as JSON:', req.body);
    return res.status(400).json({ status: 'error', message: 'invalid JSON' });
  }

  const point = {
    trackerId: data.TrackerId,
    lat: parseFloat(data.Latitude),
    lon: parseFloat(data.Longitude),
    altitude: data.Altitude,
    timestamp: data.Timestamp,
    maplink: data.Maplink,
    receivedAt: new Date().toISOString(),
  };

  if (Number.isNaN(point.lat) || Number.isNaN(point.lon)) {
    console.error('Rejected point with invalid coordinates:', data);
    return res.status(400).json({ status: 'error', message: 'invalid coordinates' });
  }

  addPoint(point);

  console.log(`[${point.receivedAt}] ${point.trackerId} -> ${point.lat}, ${point.lon}`);

  // The Quectel AT+QHTTPPOST flow just needs a 200-series response; the sketch
  // now also reads "intervalMs" back out of this ack and uses it as its next
  // delay, so a config change reaches the device on its very next transmission
  // without reflashing or restarting anything.
  res.status(200).json({ status: 'ok', intervalMs: config.intervalMs });
});

// Frontend polls these to draw the map.
// ?since=<seq> returns only points newer than that sequence number.
app.get('/api/history', (req, res) => {
  const since = Number(req.query.since);
  res.json(Number.isFinite(since) ? history.filter(p => p.seq > since) : history);
});

app.get('/api/latest', (req, res) => {
  const latest = history[history.length - 1] || null;
  const latestPhoto = photos[photos.length - 1];
  res.json(latest ? { ...latest, photoUrl: latestPhoto ? photoMeta(latestPhoto).url : null } : null);
});

// One small request that tells a client everything it needs to decide
// whether to refresh: newest seq, latest point/photo and the current delay.
app.get('/api/status', (req, res) => {
  const latest = history[history.length - 1] || null;
  const latestPhoto = photos[photos.length - 1] || null;
  res.json({
    seq,
    generation,
    intervalMs: config.intervalMs,
    serverTime: new Date().toISOString(),
    latest,
    latestPhoto: photoMeta(latestPhoto),
    pointCount: history.length,
    photoCount: photos.length,
  });
});

app.get('/api/photos', (req, res) => {
  res.json(photos.map(photoMeta).reverse());
});

app.get('/api/photos/latest.jpg', (req, res) => {
  const p = photos[photos.length - 1];
  if (!p) return res.status(404).json({ status: 'error', message: 'no photos yet' });
  res.set('Cache-Control', 'no-store').type('image/jpeg').send(p.data);
});

app.get('/api/photos/:id.jpg', (req, res) => {
  const p = photos.find(x => x.id === req.params.id);
  if (!p) return res.status(404).json({ status: 'error', message: 'photo not found (server may have restarted)' });
  // Photo ids never change content, so browsers can cache them.
  res.set('Cache-Control', 'public, max-age=86400').type('image/jpeg').send(p.data);
});

// Optional: wipe history from the browser (handy while testing)
app.post('/api/reset', (req, res) => {
  history = [];
  photos = [];
  generation = Date.now();
  res.json({ status: 'cleared' });
});

// ---------------------------------------------------------------------
// DELAY CONFIG API
// ---------------------------------------------------------------------

// Website (and anyone else) can read the current delay.
app.get('/api/config', (req, res) => {
  res.json(config);
});

// Website dropdown posts here to change the delay live.
// Body: { "intervalMs": 10000 }
app.post('/api/config', (req, res) => {
  let body;
  try {
    body = typeof req.body === 'string' ? JSON.parse(req.body) : req.body;
  } catch (err) {
    return res.status(400).json({ status: 'error', message: 'invalid JSON' });
  }

  const intervalMs = Number(body && body.intervalMs);

  if (
    !Number.isFinite(intervalMs) ||
    intervalMs < MIN_INTERVAL_MS ||
    intervalMs > MAX_INTERVAL_MS
  ) {
    return res.status(400).json({
      status: 'error',
      message: `intervalMs must be a number between ${MIN_INTERVAL_MS} and ${MAX_INTERVAL_MS}`,
    });
  }

  config = { intervalMs };

  try {
    saveConfig(config);
  } catch (err) {
    console.error('Could not persist config.json (interval is still updated in memory):', err);
  }

  console.log(`Delay updated -> ${intervalMs} ms`);
  res.json({ status: 'ok', ...config });
});

// Oversized uploads land here instead of crashing with an HTML error page.
app.use((err, req, res, next) => {
  if (err && err.type === 'entity.too.large') {
    return res.status(413).json({ status: 'error', message: 'upload too large' });
  }
  console.error(err);
  res.status(500).json({ status: 'error', message: 'server error' });
});

app.listen(PORT, '0.0.0.0', () => {
  console.log(`Drone tracker server listening on http://0.0.0.0:${PORT}`);
  console.log(`Open http://localhost:${PORT} in a browser to view the live map`);
  console.log(`Current tracker delay: ${config.intervalMs} ms`);
});
