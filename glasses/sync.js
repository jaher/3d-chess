/*
 * 3D Chess — Meta Ray-Ban Display Web App: move-sync client.
 *
 * A thin WebSocket client that links the glasses game to a desktop (or another
 * browser) through the sync-server.js relay, so moves on the glasses match the
 * desktop and vice versa. The glasses cannot use Bluetooth (the Web App sandbox
 * has no Web Bluetooth API — only Neural Band, IMU, GPS, storage), so the link
 * is a WebSocket; the desktop joins the same relay room over raw TCP.
 *
 * Attaches to window.ChessSync. The relay forwards each JSON message to the
 * other members of the room:
 *   sendMove('e2e4')        -> { type:'move', uci }
 *   sendReset(amWhite)      -> { type:'reset', fromWhite: amWhite }
 * Incoming messages fire the handlers passed to connect().
 */
(function (global) {
  'use strict';

  let ws = null;
  let h = {};            // handlers
  let cur = { room: null, name: null, url: null };

  function connect(url, room, name, handlers) {
    disconnect();
    h = handlers || {};
    cur = { room, name, url };
    try {
      ws = new WebSocket(url);
    } catch (e) {
      if (h.onStatus) h.onStatus('error');
      return;
    }
    ws.onopen = () => {
      send({ type: 'join', room, name });
      if (h.onStatus) h.onStatus('connected');
    };
    ws.onclose = () => { if (h.onStatus) h.onStatus('closed'); };
    ws.onerror = () => { if (h.onStatus) h.onStatus('error'); };
    ws.onmessage = (ev) => {
      let msg; try { msg = JSON.parse(ev.data); } catch (_) { return; }
      if (msg.type === 'move' && h.onMove) h.onMove(msg.uci);
      else if (msg.type === 'reset' && h.onReset) h.onReset(!!msg.fromWhite);
      else if (msg.type === 'role' && h.onRole) h.onRole(!!msg.initiator);
      else if (msg.type === 'peer' && h.onPeer) h.onPeer(msg.name, msg.joined);
    };
  }

  function send(obj) {
    if (ws && ws.readyState === 1) { try { ws.send(JSON.stringify(obj)); } catch (_) {} }
  }
  function sendMove(uci) { send({ type: 'move', uci }); }
  function sendReset(amWhite) { send({ type: 'reset', fromWhite: amWhite }); }
  function isConnected() { return !!ws && ws.readyState === 1; }
  function disconnect() {
    if (ws) { try { ws.onclose = null; ws.close(); } catch (_) {} ws = null; }
  }

  global.ChessSync = { connect, disconnect, sendMove, sendReset, isConnected, info: () => cur };
})(typeof window !== 'undefined' ? window : globalThis);
