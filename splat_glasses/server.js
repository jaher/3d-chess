/*
 * splat_glasses — phone-to-glasses Gaussian-splat world streamer.
 *
 * One dependency-free Node process (same philosophy as ../glasses/sync-server.js)
 * that glues four things together:
 *
 *   1. Static pages: the phone capture page (/) and the glasses viewer
 *      (/glasses). The glasses are a Meta Ray-Ban Display Web App: 600x600
 *      viewport, no WebGL guarantees, Neural Band input arrives as ordinary
 *      ArrowUp/Down/Left/Right + Enter + Escape `keydown` events (see
 *      ../glasses/app.js — same input model).
 *
 *   2. World Labs Marble API proxy (key stays server-side in
 *      $WORLD_LABS_API_KEY, never sent to a page, never logged):
 *        POST /api/generate     photo (base64) -> worlds:generate operation
 *        GET  /api/operations/x poll a generate operation
 *        GET  /api/worlds       list existing generated worlds
 *      The phone photo goes up as an inline data_base64 image prompt, so no
 *      public hosting of the upload is needed.
 *
 *   3. Renderer manager: spawns ./splat_render (CPU EWA splat rasterizer,
 *      see renderer.cpp) as a persistent child per loaded world. The server
 *      owns the camera pose; the child gets "P x y z yaw pitch" lines and
 *      answers with JPEG frames. Latest-wins pacing: while a frame is in
 *      flight, pose updates just overwrite the pending pose, so the stream
 *      never falls behind the input.
 *
 *   4. Frame fan-out to the glasses:
 *        - WebSocket binary JPEG frames (primary; lowest latency, same
 *          socket carries nav input upstream)
 *        - GET /stream.mjpg   multipart MJPEG fallback / curl-friendly
 *        - GET /frame.jpg     one-shot latest frame (smoke test)
 *
 * Run:  node splat_glasses/server.js [port=8095]
 * Real glasses require HTTPS/WSS — put this behind a TLS reverse proxy
 * (same constraint and solution as the chess glasses app).
 */
'use strict';

const http = require('http');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { spawn } = require('child_process');

const PORT = parseInt(process.argv[2] || '8095', 10);
const ROOT = __dirname;
const WORLDS_DIR = path.join(ROOT, 'worlds');
const RENDERER = path.join(ROOT, 'splat_render');
const API_BASE = 'https://api.worldlabs.ai/marble/v1';
const API_KEY = process.env.WORLD_LABS_API_KEY || '';
const FRAME_W = parseInt(process.env.SPLAT_FRAME_W || '480', 10);
const FRAME_H = parseInt(process.env.SPLAT_FRAME_H || '480', 10);
// Default world so the glasses page works before any generation: prefer the
// checked-in datacenter splat, else the first cached download.
const DEFAULT_SPZ = process.env.SPLAT_DEFAULT ||
    path.join(ROOT, '..', 'world_labs', 'datacenter', 'splat_500k.spz');

if (!API_KEY) log('WARN: WORLD_LABS_API_KEY not set — generation disabled, local worlds still work');
fs.mkdirSync(WORLDS_DIR, { recursive: true });

function log(...a) { console.log(new Date().toISOString().slice(11, 19), ...a); }

// ---------------------------------------------------------------------------
// Camera + navigation. The server owns the pose; Neural Band events from the
// glasses (or arrows on any keyboard) mutate it. Discrete steps per event —
// key-repeat gives continuous motion.
// ---------------------------------------------------------------------------
const HOME = { x: 0, y: 0, z: 0, yaw: 0, pitch: 0 };
let cam = { ...HOME };
const TURN = 12 * Math.PI / 180;   // per left/right event
const STEP = 0.35;                 // metres per fwd/back event (Marble worlds are ~metric)

function nav(action) {
  switch (action) {
    case 'left':  cam.yaw += TURN; break;
    case 'right': cam.yaw -= TURN; break;
    case 'fwd':   cam.x += -Math.sin(cam.yaw) * STEP; cam.z += -Math.cos(cam.yaw) * STEP; break;
    case 'back':  cam.x +=  Math.sin(cam.yaw) * STEP; cam.z +=  Math.cos(cam.yaw) * STEP; break;
    case 'up':    cam.pitch = Math.min( 1.2, cam.pitch + 8 * Math.PI / 180); break;
    case 'down':  cam.pitch = Math.max(-1.2, cam.pitch - 8 * Math.PI / 180); break;
    case 'reset': cam = { ...HOME }; break;
    default: return;
  }
  requestFrame();
}

// ---------------------------------------------------------------------------
// Renderer child management + latest-wins frame pump.
// ---------------------------------------------------------------------------
let child = null;          // current splat_render process
let childBuf = Buffer.alloc(0);
let inFlight = false;      // a pose was sent, frame not yet received
let dirty = false;         // pose changed while a frame was in flight
let latestFrame = null;    // Buffer of the newest JPEG
let worldName = 'none';
let lastFrameMs = 0;       // render+encode+ipc time of the last frame

function loadWorld(spzPath, name) {
  if (child) { try { child.stdin.write('Q\n'); } catch (_) {} try { child.kill(); } catch (_) {} }
  child = spawn(RENDERER, [spzPath, String(FRAME_W), String(FRAME_H)],
                { stdio: ['pipe', 'pipe', 'pipe'] });
  childBuf = Buffer.alloc(0);
  inFlight = false; dirty = false;
  worldName = name || path.basename(spzPath);
  cam = { ...HOME };
  const started = child;
  child.stdout.on('data', (d) => { if (child === started) onRendererData(d); });
  child.stderr.on('data', (d) => process.stderr.write(`[splat_render] ${d}`));
  child.on('exit', (code) => { if (child === started) { log(`renderer exited (${code})`); child = null; } });
  log(`world loaded: ${worldName} (${spzPath}) at ${FRAME_W}x${FRAME_H}`);
  broadcastJson({ type: 'world', name: worldName });
  requestFrame();
}

function requestFrame() {
  if (!child) return;
  if (inFlight) { dirty = true; return; }
  inFlight = true;
  requestFrame.t0 = Date.now();
  try {
    child.stdin.write(`P ${cam.x.toFixed(4)} ${cam.y.toFixed(4)} ${cam.z.toFixed(4)} ` +
                      `${cam.yaw.toFixed(5)} ${cam.pitch.toFixed(5)}\n`);
  } catch (_) { inFlight = false; }
}

function onRendererData(d) {
  childBuf = Buffer.concat([childBuf, d]);
  for (;;) {
    const nl = childBuf.indexOf(0x0a);
    if (nl < 0) return;
    const header = childBuf.slice(0, nl).toString();
    const m = /^F (\d+)$/.exec(header);
    if (!m) { childBuf = childBuf.slice(nl + 1); continue; }
    const n = parseInt(m[1], 10);
    if (childBuf.length < nl + 1 + n) return;         // wait for full frame
    latestFrame = childBuf.slice(nl + 1, nl + 1 + n);
    childBuf = childBuf.slice(nl + 1 + n);
    lastFrameMs = Date.now() - (requestFrame.t0 || Date.now());
    inFlight = false;
    fanOutFrame(latestFrame);
    if (dirty) { dirty = false; requestFrame(); }
  }
}

// ---------------------------------------------------------------------------
// Frame fan-out: WS viewers + MJPEG clients.
// ---------------------------------------------------------------------------
const viewers = new Set();      // ws clients that asked for frames
const mjpegClients = new Set(); // http responses in multipart mode

function fanOutFrame(jpeg) {
  for (const v of viewers) v.sendBinary(jpeg);
  for (const res of mjpegClients) {
    try {
      res.write(`--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${jpeg.length}\r\n\r\n`);
      res.write(jpeg);
      res.write('\r\n');
    } catch (_) { mjpegClients.delete(res); }
  }
}

function broadcastJson(obj) {
  for (const v of viewers) v.sendJson(obj);
}

// ---------------------------------------------------------------------------
// Marble API proxy (server-side key; never logged).
// ---------------------------------------------------------------------------
async function marble(pathname, method, body) {
  const res = await fetch(`${API_BASE}${pathname}`, {
    method,
    headers: { 'WLT-Api-Key': API_KEY, 'Content-Type': 'application/json' },
    body: body ? JSON.stringify(body) : undefined,
  });
  const text = await res.text();
  return { status: res.status, text };
}

async function downloadSpz(url, dest) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`spz download failed: HTTP ${res.status}`);
  const buf = Buffer.from(await res.arrayBuffer());
  fs.writeFileSync(dest, buf);
  return buf.length;
}

// ---------------------------------------------------------------------------
// HTTP server: static + API + streams.
// ---------------------------------------------------------------------------
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css',
               '.json': 'application/json', '.png': 'image/png', '.jpg': 'image/jpeg',
               '.svg': 'image/svg+xml' };

function serveStatic(res, file) {
  const p = path.join(ROOT, file);
  if (!p.startsWith(ROOT) || !fs.existsSync(p) || !fs.statSync(p).isFile()) {
    res.writeHead(404); res.end('not found'); return;
  }
  res.writeHead(200, { 'Content-Type': MIME[path.extname(p)] || 'application/octet-stream' });
  res.end(fs.readFileSync(p));
}

function readBody(req, limitMb = 25) {
  return new Promise((resolve, reject) => {
    const chunks = []; let total = 0;
    req.on('data', (d) => {
      total += d.length;
      if (total > limitMb * 1024 * 1024) { reject(new Error('body too large')); req.destroy(); return; }
      chunks.push(d);
    });
    req.on('end', () => resolve(Buffer.concat(chunks)));
    req.on('error', reject);
  });
}

function sendJson(res, status, obj) {
  res.writeHead(status, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify(obj));
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x');
  try {
    // --- pages -------------------------------------------------------------
    if (req.method === 'GET' && (url.pathname === '/' || url.pathname === '/phone')) return serveStatic(res, 'phone.html');
    if (req.method === 'GET' && url.pathname === '/glasses') return serveStatic(res, 'glasses.html');
    if (req.method === 'GET' && /^\/[\w.-]+\.(css|js|json|png|svg)$/.test(url.pathname)) return serveStatic(res, url.pathname.slice(1));

    // --- frame streams -----------------------------------------------------
    if (req.method === 'GET' && url.pathname === '/frame.jpg') {
      if (!latestFrame) { res.writeHead(503); res.end('no frame yet'); return; }
      res.writeHead(200, { 'Content-Type': 'image/jpeg', 'Cache-Control': 'no-store' });
      res.end(latestFrame);
      return;
    }
    if (req.method === 'GET' && url.pathname === '/stream.mjpg') {
      res.writeHead(200, {
        'Content-Type': 'multipart/x-mixed-replace; boundary=frame',
        'Cache-Control': 'no-store', 'Connection': 'close',
      });
      mjpegClients.add(res);
      req.on('close', () => mjpegClients.delete(res));
      if (latestFrame) fanOutFrame(latestFrame);
      else requestFrame();
      return;
    }

    // --- status / nav (nav also available over plain HTTP for debugging) ----
    if (req.method === 'GET' && url.pathname === '/api/status') {
      return sendJson(res, 200, {
        world: worldName, rendererUp: !!child, cam,
        frameMs: lastFrameMs, viewers: viewers.size, mjpeg: mjpegClients.size,
        size: [FRAME_W, FRAME_H],
      });
    }
    if (req.method === 'POST' && url.pathname === '/api/nav') {
      const body = JSON.parse((await readBody(req)).toString() || '{}');
      nav(String(body.action || ''));
      return sendJson(res, 200, { ok: true, cam });
    }

    // --- Marble proxy --------------------------------------------------------
    if (req.method === 'POST' && url.pathname === '/api/generate') {
      if (!API_KEY) return sendJson(res, 500, { error: 'WORLD_LABS_API_KEY not set on server' });
      const body = JSON.parse((await readBody(req)).toString());
      if (!body.image_base64) return sendJson(res, 400, { error: 'image_base64 required' });
      const gen = {
        display_name: String(body.display_name || 'Glasses world'),
        model: 'marble-1.1',
        world_prompt: {
          type: 'image',
          image_prompt: {
            source: 'data_base64',
            data_base64: body.image_base64,
            extension: String(body.extension || 'jpg'),
          },
          ...(body.text_prompt ? { text_prompt: String(body.text_prompt) } : {}),
        },
      };
      const r = await marble('/worlds:generate', 'POST', gen);
      log(`generate -> HTTP ${r.status}`);
      res.writeHead(r.status, { 'Content-Type': 'application/json' });
      res.end(r.text);
      return;
    }
    if (req.method === 'GET' && /^\/api\/operations\/[\w-]+$/.test(url.pathname)) {
      const id = url.pathname.split('/').pop();
      const r = await marble(`/operations/${id}`, 'GET');
      res.writeHead(r.status, { 'Content-Type': 'application/json' });
      res.end(r.text);
      return;
    }
    if (req.method === 'GET' && url.pathname === '/api/worlds') {
      const r = await marble('/worlds:list', 'POST', { page_size: 20 });
      res.writeHead(r.status, { 'Content-Type': 'application/json' });
      res.end(r.text);
      return;
    }

    // --- load a world into the renderer --------------------------------------
    if (req.method === 'POST' && url.pathname === '/api/load') {
      const body = JSON.parse((await readBody(req)).toString());
      const spzUrl = String(body.spz_url || '');
      const name = String(body.name || 'world');
      if (!/^https:\/\/[\w.-]+\.worldlabs\.ai\//.test(spzUrl)) {
        return sendJson(res, 400, { error: 'spz_url must be a worldlabs.ai CDN URL' });
      }
      const dest = path.join(WORLDS_DIR,
        crypto.createHash('sha1').update(spzUrl).digest('hex').slice(0, 16) + '.spz');
      if (!fs.existsSync(dest)) {
        log(`downloading ${name} ...`);
        const n = await downloadSpz(spzUrl, dest);
        log(`downloaded ${name}: ${(n / 1e6).toFixed(1)} MB`);
      }
      loadWorld(dest, name);
      return sendJson(res, 200, { ok: true, world: worldName });
    }

    res.writeHead(404); res.end('not found');
  } catch (e) {
    log('request error:', e.message);
    try { sendJson(res, 500, { error: e.message }); } catch (_) {}
  }
});

// ---------------------------------------------------------------------------
// WebSocket (RFC 6455, minimal — extended from ../glasses/sync-server.js with
// binary frames and 64-bit lengths for JPEG payloads).
// ---------------------------------------------------------------------------
const WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

server.on('upgrade', (req, socket) => {
  const key = req.headers['sec-websocket-key'];
  if (!key) { socket.destroy(); return; }
  const accept = crypto.createHash('sha1').update(key + WS_GUID).digest('base64');
  socket.write('HTTP/1.1 101 Switching Protocols\r\n' +
               'Upgrade: websocket\r\nConnection: Upgrade\r\n' +
               `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
  socket.setNoDelay(true);

  const client = {
    sendJson: (obj) => { try { socket.write(wsFrame(Buffer.from(JSON.stringify(obj)), 0x1)); } catch (_) {} },
    sendBinary: (buf) => { try { socket.write(wsFrame(buf, 0x2)); } catch (_) {} },
  };
  let buf = Buffer.alloc(0);
  socket.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    let frame;
    while ((frame = wsParse(buf))) {
      buf = frame.rest;
      if (frame.opcode === 0x8) { socket.end(); return; }
      if (frame.opcode === 0x9) { socket.write(wsFrame(frame.payload, 0xA)); continue; }
      if (frame.opcode === 0x1) onWsMessage(client, frame.payload.toString());
    }
  });
  const drop = () => { viewers.delete(client); };
  socket.on('close', drop);
  socket.on('error', drop);
});

function onWsMessage(client, text) {
  let msg; try { msg = JSON.parse(text); } catch (_) { return; }
  if (msg.type === 'hello') {
    if (msg.role === 'viewer') {
      viewers.add(client);
      client.sendJson({ type: 'world', name: worldName });
      if (latestFrame) client.sendBinary(latestFrame);
      else requestFrame();
      log(`viewer connected (${viewers.size})`);
    }
    return;
  }
  if (msg.type === 'nav') nav(String(msg.action || ''));
}

function wsFrame(payload, opcode) {
  const len = payload.length;
  let header;
  if (len < 126) {
    header = Buffer.from([0x80 | opcode, len]);
  } else if (len < 65536) {
    header = Buffer.alloc(4); header[0] = 0x80 | opcode; header[1] = 126;
    header.writeUInt16BE(len, 2);
  } else {
    header = Buffer.alloc(10); header[0] = 0x80 | opcode; header[1] = 127;
    header.writeBigUInt64BE(BigInt(len), 2);
  }
  return Buffer.concat([header, payload]);
}
function wsParse(buf) {
  if (buf.length < 2) return null;
  const opcode = buf[0] & 0x0f;
  const masked = (buf[1] & 0x80) !== 0;
  let len = buf[1] & 0x7f;
  let off = 2;
  if (len === 126) { if (buf.length < 4) return null; len = buf.readUInt16BE(2); off = 4; }
  else if (len === 127) { if (buf.length < 10) return null; len = Number(buf.readBigUInt64BE(2)); off = 10; }
  const maskLen = masked ? 4 : 0;
  if (buf.length < off + maskLen + len) return null;
  let payload = buf.slice(off + maskLen, off + maskLen + len);
  if (masked) {
    const mask = buf.slice(off, off + 4);
    payload = Buffer.from(payload);
    for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
  }
  return { opcode, payload, rest: buf.slice(off + maskLen + len) };
}

// ---------------------------------------------------------------------------
server.listen(PORT, () => {
  log(`splat_glasses server on http://0.0.0.0:${PORT}  (phone: /  glasses: /glasses)`);
  if (fs.existsSync(RENDERER)) {
    if (fs.existsSync(DEFAULT_SPZ)) loadWorld(DEFAULT_SPZ, path.basename(path.dirname(DEFAULT_SPZ)));
    else log('no default world; POST /api/load or generate one from the phone page');
  } else {
    log(`renderer missing — build it first: make -C splat_glasses`);
  }
});
