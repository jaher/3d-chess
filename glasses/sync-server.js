/*
 * 3D Chess — move-sync relay for glasses <-> desktop.
 *
 * WHY THIS EXISTS / WHY NOT BLUETOOTH: the Meta Ray-Ban Display Web App sandbox
 * exposes only Neural Band, IMU, GPS, and storage to JavaScript — there is no
 * Web Bluetooth API — so the glasses cannot open a Bluetooth link. They CAN do
 * standard WebSocket networking, so glasses<->desktop move sync goes over the
 * network. This relay bridges two transports in a shared "room":
 *
 *   - WebSocket  (for browsers / the glasses Web App — a browser's only
 *                 full-duplex transport)
 *   - raw TCP    (for the native C++ desktop — a few lines of BSD sockets,
 *                 see net_sync.cpp)
 *
 * Both speak the same newline-delimited JSON; the relay forwards every message
 * to the OTHER members of the same room, regardless of transport. Dependency-
 * free pure Node (no npm, no package.json) so it runs anywhere with `node`.
 *
 * Protocol (one JSON object per message; TCP messages are newline-terminated):
 *   { "type":"join", "room":"1234", "name":"glasses" }
 *   { "type":"move", "uci":"e2e4" }
 *   { "type":"reset", "white":"glasses" }      // new game / who is white
 *   { "type":"hello" } / { "type":"peer", "name":..., "joined":true|false }
 *
 * Run:  node glasses/sync-server.js [wsPort=890] [tcpPort=8091]
 * For real glasses use this must sit behind WSS (the glasses require HTTPS/WSS);
 * for local testing ws:// on localhost is fine.
 */
'use strict';
const http = require('http');
const net = require('net');
const crypto = require('crypto');

const WS_PORT = parseInt(process.argv[2] || '8090', 10);
const TCP_PORT = parseInt(process.argv[3] || '8091', 10);
const WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

// room -> Set of client objects { send(obj), name, room }
const rooms = new Map();

function joinRoom(client, room, name) {
  client.room = room; client.name = name || 'peer';
  const wasEmpty = !rooms.has(room) || rooms.get(room).size === 0;
  if (!rooms.has(room)) rooms.set(room, new Set());
  rooms.get(room).add(client);
  // Tell the joiner its role: the first into an empty room is the colour
  // authority (initiator); later joiners follow its reset. Makes the link
  // order-independent — either glasses or desktop can connect first.
  client.send({ type: 'role', initiator: wasEmpty });
  broadcast(client, { type: 'peer', name: client.name, joined: true });
  log(`+ ${client.name} joined room ${room} as ${wasEmpty ? 'initiator' : 'follower'} (${rooms.get(room).size} in room)`);
}
function leaveRoom(client) {
  const set = rooms.get(client.room);
  if (!set) return;
  set.delete(client);
  broadcast(client, { type: 'peer', name: client.name, joined: false });
  if (set.size === 0) rooms.delete(client.room);
  log(`- ${client.name} left room ${client.room}`);
}
function broadcast(from, obj) {
  const set = rooms.get(from.room);
  if (!set) return;
  for (const c of set) if (c !== from) c.send(obj);
}
function onMessage(client, text) {
  let msg; try { msg = JSON.parse(text); } catch (_) { return; }
  if (msg.type === 'join') { joinRoom(client, String(msg.room || 'lobby'), msg.name); return; }
  if (!client.room) return;                 // must join first
  // move / reset / anything else: forward verbatim to the rest of the room.
  broadcast(client, msg);
}
function log(...a) { console.log(new Date().toISOString().slice(11, 19), ...a); }

// --- WebSocket transport (RFC 6455, minimal: small text frames) -------------
const server = http.createServer((req, res) => { res.writeHead(200); res.end('chess sync relay\n'); });
server.on('upgrade', (req, socket) => {
  const key = req.headers['sec-websocket-key'];
  if (!key) { socket.destroy(); return; }
  const accept = crypto.createHash('sha1').update(key + WS_GUID).digest('base64');
  socket.write(
    'HTTP/1.1 101 Switching Protocols\r\n' +
    'Upgrade: websocket\r\nConnection: Upgrade\r\n' +
    `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);

  const client = {
    name: 'ws', room: null,
    send: (obj) => { try { socket.write(wsFrame(JSON.stringify(obj))); } catch (_) {} },
  };
  let buf = Buffer.alloc(0);
  socket.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    let frame;
    while ((frame = wsParse(buf))) {
      buf = frame.rest;
      if (frame.opcode === 0x8) { socket.end(); return; }            // close
      if (frame.opcode === 0x9) { socket.write(wsFrame(frame.payload.toString(), 0xA)); continue; } // ping->pong
      if (frame.opcode === 0x1 || frame.opcode === 0x0) onMessage(client, frame.payload.toString());
    }
  });
  socket.on('close', () => leaveRoom(client));
  socket.on('error', () => leaveRoom(client));
});
function wsFrame(str, opcode = 0x1) {
  const payload = Buffer.from(str);
  const len = payload.length;
  let header;
  if (len < 126) { header = Buffer.from([0x80 | opcode, len]); }
  else { header = Buffer.alloc(4); header[0] = 0x80 | opcode; header[1] = 126; header.writeUInt16BE(len, 2); }
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

// --- raw TCP transport (newline-delimited JSON) -----------------------------
const tcp = net.createServer((socket) => {
  const client = {
    name: 'tcp', room: null,
    send: (obj) => { try { socket.write(JSON.stringify(obj) + '\n'); } catch (_) {} },
  };
  let acc = '';
  socket.setEncoding('utf8');
  socket.on('data', (d) => {
    acc += d;
    let nl;
    while ((nl = acc.indexOf('\n')) >= 0) { const line = acc.slice(0, nl); acc = acc.slice(nl + 1); if (line.trim()) onMessage(client, line); }
  });
  socket.on('close', () => leaveRoom(client));
  socket.on('error', () => leaveRoom(client));
});

server.listen(WS_PORT, () => log(`WebSocket relay on ws://0.0.0.0:${WS_PORT}`));
tcp.listen(TCP_PORT, () => log(`TCP relay on tcp://0.0.0.0:${TCP_PORT}`));
