// peer-bridge.js — online multiplayer for 3D Chess over WebRTC (PeerJS).
//
// One peer connection carries BOTH the game (a reliable data channel of
// UCI moves) and the webcam (a media call). Mirrors stockfish-bridge.js:
// it exposes a single global (window.OnlineChess) that the C++/WASM side
// reaches via Module.ccall, and it calls back into WASM through the
// exported chess_start_network_game / on_remote_move_from_js entry points.
//
// Roles: the HOST creates a peer and shares its id as a room code and
// plays White; the JOINER pastes that code, connects, and plays Black.
// Signalling uses the free public PeerJS broker — fine for casual play;
// swap in your own PeerServer for reliability/scale.

(function () {
  'use strict';

  var peer = null;        // PeerJS Peer
  var conn = null;        // data connection (moves)
  var localStream = null; // our camera+mic MediaStream
  var isHost = false;
  var remoteId = null;    // the opponent's peer id (for the media call)

  function $(id) { return document.getElementById(id); }

  function setStatus(s) {
    var e = $('online-status');
    if (e) e.textContent = s;
    console.log('[net]', s);
  }

  function peerLibReady() {
    if (typeof Peer === 'undefined') {
      setStatus('Online play unavailable (PeerJS failed to load).');
      return false;
    }
    return true;
  }

  // ---- WASM hand-off ----
  function startLocalGame(localWhite) {
    if (typeof Module !== 'undefined' && Module.ccall) {
      Module.ccall('chess_start_network_game', null, ['number'],
                   [localWhite ? 1 : 0]);
    }
    var panel = $('online-panel'); if (panel) panel.style.display = 'none';
    var btn = $('online-btn');     if (btn)   btn.style.display = 'none';
  }

  // ---- webcam ----
  function getCam(cb) {
    if (localStream) { cb(localStream); return; }
    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
      setStatus('Camera not supported by this browser.'); cb(null); return;
    }
    navigator.mediaDevices.getUserMedia({ video: true, audio: true })
      .then(function (s) {
        localStream = s;
        var lv = $('local-cam');
        if (lv) { lv.srcObject = s; lv.style.display = 'block'; }
        cb(s);
      })
      .catch(function (e) {
        // Play can still proceed without video.
        setStatus('Playing without camera (' + (e && e.name ? e.name : e) + ').');
        cb(null);
      });
  }

  function showRemote(stream) {
    var rv = $('remote-cam');
    if (rv) { rv.srcObject = stream; rv.style.display = 'block'; }
  }

  // The joiner places the media call (it knows the host's id); the host
  // answers via peer.on('call').
  function callPeerWithCam() {
    getCam(function (s) {
      if (s && peer && remoteId) {
        var call = peer.call(remoteId, s);
        if (call) call.on('stream', showRemote);
      }
    });
  }

  function answerCall(call) {
    getCam(function (s) {
      call.answer(s || undefined);
      call.on('stream', showRemote);
    });
  }

  // ---- data connection (moves) ----
  function wireDataConn(c) {
    conn = c;
    conn.on('open', function () {
      remoteId = conn.peer;
      setStatus('Connected — game on.');
      startLocalGame(isHost);          // host = White, joiner = Black
      if (!isHost) callPeerWithCam();  // joiner initiates video
      else getCam(function () {});     // host pre-warms its camera to answer
    });
    conn.on('data', function (msg) {
      if (msg && msg.type === 'move' &&
          typeof Module !== 'undefined' && Module.ccall) {
        Module.ccall('on_remote_move_from_js', null, ['string'], [msg.uci]);
      }
    });
    conn.on('close', function () { setStatus('Opponent disconnected.'); });
    conn.on('error', function (e) { setStatus('Connection error: ' + e); });
  }

  function createPeer(onOpen) {
    peer = new Peer();   // random id from the public broker
    peer.on('open', onOpen);
    peer.on('connection', wireDataConn);  // host receives the joiner
    peer.on('call', answerCall);          // host answers the video call
    peer.on('error', function (e) {
      setStatus('Peer error: ' + (e && e.type ? e.type : e));
    });
  }

  window.OnlineChess = {
    host: function () {
      if (!peerLibReady()) return;
      isHost = true;
      setStatus('Creating room…');
      createPeer(function (id) {
        var codeEl = $('online-code');
        if (codeEl) {
          codeEl.textContent = id;
          codeEl.style.display = 'block';
        }
        setStatus('Share this code, then wait for your opponent:');
      });
    },
    join: function (code) {
      if (!peerLibReady()) return;
      code = (code || '').trim();
      if (!code) { setStatus('Paste the host code first.'); return; }
      isHost = false;
      setStatus('Connecting…');
      createPeer(function () {
        remoteId = code;
        wireDataConn(peer.connect(code, { reliable: true }));
        callPeerWithCam();   // joiner sends video too
      });
    },
    // Called from C++ (plat_send_move) after the local player moves.
    sendMove: function (uci) {
      if (conn && conn.open) conn.send({ type: 'move', uci: uci });
    }
  };
})();
