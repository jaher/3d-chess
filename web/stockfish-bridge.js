// Bridges the WASM C++ code to the Stockfish.js Web Worker.
//
// nmrugg/stockfish.js v18.0.0 lite-single-threaded build is loaded as a
// Worker. We post UCI commands and route the textual responses back to the
// WASM side via Module.ccall.
//
// Important design point: requests are SERIALIZED via an explicit queue.
// Only one Stockfish search runs at a time. The desktop subprocess
// implementation in ai_player.cpp serializes naturally through the engine's
// stdin/stdout pipe; here the Web Worker accepts messages eagerly and we
// have to serialize on the JS side, otherwise the bridge can attribute a
// `bestmove` from one search to the wrong request type and the C++ side
// hangs waiting for a result that never arrives.

window.StockfishBridge = (function () {
  const worker = new Worker('stockfish/stockfish.js');

  // Each queued entry: { kind: 'move'|'eval', fen, movetime, idx }
  const queue = [];
  let active = null;          // currently-running entry, or null
  let bestEval = 0;           // running best eval score for the active eval search
  let handshakeDone = false;  // becomes true once 'readyok' arrives

  function safe_ccall(name, retType, argTypes, args) {
    if (typeof Module === 'undefined' || typeof Module.ccall !== 'function') {
      console.warn('[sf-bridge] Module.ccall not ready for', name);
      return;
    }
    try {
      Module.ccall(name, retType, argTypes, args);
    } catch (e) {
      console.error('[sf-bridge]', name, 'ccall failed:', e);
    }
  }

  function fenSideToMove(fen) {
    const parts = fen.split(' ');
    return parts.length >= 2 && parts[1] === 'b';
  }

  function startNext() {
    if (active || queue.length === 0) return;
    active = queue.shift();
    bestEval = 0;
    worker.postMessage('ucinewgame');
    worker.postMessage('position fen ' + active.fen);
    worker.postMessage('go movetime ' + active.movetime);
  }

  function finishActive(handler) {
    handler();
    active = null;
    startNext();
  }

  worker.onmessage = function (e) {
    const line = e.data;
    if (typeof line !== 'string') return;

    // Discard handshake responses (uciok / readyok / option ...).
    if (!handshakeDone) {
      if (line === 'readyok') handshakeDone = true;
      return;
    }

    if (!active) {
      // Stray output (residual info lines after a finished search). Ignore.
      return;
    }

    if (active.kind === 'move') {
      if (line.startsWith('bestmove ')) {
        const tokens = line.split(' ');
        let uci = tokens[1] || '';
        if (uci === '(none)' || uci === '0000') uci = '';
        // Drop the 5th promotion char; execute_move auto-queens.
        if (uci.length > 4) uci = uci.substring(0, 4);
        finishActive(function () {
          safe_ccall('on_ai_move_from_js', null, ['string'], [uci]);
        });
      }
      return;
    }

    if (active.kind === 'eval') {
      // Track the latest score from the search's info lines.
      const m = line.match(/score (cp|mate) (-?\d+)/);
      if (m) {
        let cp = parseInt(m[2], 10);
        if (m[1] === 'mate') {
          const sign = cp >= 0 ? 1 : -1;
          cp = sign * (30000 - Math.abs(cp));
        }
        bestEval = cp;
      }
      if (line.startsWith('bestmove ')) {
        // Negate if black-to-move so the C++ side gets white-relative cp.
        const sideBlack = fenSideToMove(active.fen);
        const cp = sideBlack ? -bestEval : bestEval;
        const idx = active.idx;
        finishActive(function () {
          safe_ccall('on_eval_from_js', null, ['number', 'number'], [cp, idx]);
        });
      }
      return;
    }
  };

  // UCI handshake — fire-and-forget; queue requests will start streaming
  // after handshakeDone is set, but we don't strictly block on it because
  // the worker processes messages in order anyway.
  worker.postMessage('uci');
  worker.postMessage('setoption name UCI_LimitStrength value true');
  worker.postMessage('setoption name UCI_Elo value 1400');
  worker.postMessage('isready');

  return {
    requestMove: function (fen, movetime) {
      queue.push({ kind: 'move', fen: fen, movetime: movetime, idx: -1 });
      startNext();
    },
    requestEval: function (fen, movetime, idx) {
      queue.push({ kind: 'eval', fen: fen, movetime: movetime, idx: idx });
      startNext();
    },
    // Update the engine's UCI_Elo. Takes effect on the next `go` — the
    // worker queues these messages after any in-flight search completes.
    // Defensive clamp: the lite-net single-threaded build accepts a
    // narrower range than mainline Stockfish and silently clamps anyway.
    setElo: function (elo) {
      elo = Math.max(1320, Math.min(3190, elo | 0));
      worker.postMessage('setoption name UCI_LimitStrength value true');
      worker.postMessage('setoption name UCI_Elo value ' + elo);
    },
  };
})();
