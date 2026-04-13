// Bridges the WASM C++ code to the Stockfish.js Web Worker.
//
// nmrugg/stockfish.js v18.0.0 lite-single-threaded build is loaded as a
// Worker. We post UCI commands and route the textual responses to either
// the move-result handler or the eval-result handler depending on what was
// requested. Result delivery into the WASM side is done via Module.ccall
// to the EMSCRIPTEN_KEEPALIVE callbacks declared in web/ai_player_web.cpp.

window.StockfishBridge = (function () {
  const worker = new Worker('stockfish/stockfish.js');

  let mode = null;            // null | 'move' | 'eval'
  let pendingMoveTime = 800;  // last requested movetime (informational)
  let pendingIdx = -1;
  let bestEval = 0;           // centipawns from side-to-move perspective
  let sideToMoveBlack = false;

  function safe_ccall(name, retType, argTypes, args) {
    if (typeof Module === 'undefined' || typeof Module.ccall !== 'function') {
      console.warn('Module.ccall not ready for', name);
      return;
    }
    try {
      Module.ccall(name, retType, argTypes, args);
    } catch (e) {
      console.error(name, 'ccall failed:', e);
    }
  }

  worker.onmessage = function (e) {
    let line = e.data;
    if (typeof line !== 'string') return;
    // console.debug('SF:', line);

    if (mode === 'move') {
      if (line.startsWith('bestmove ')) {
        const tokens = line.split(' ');
        let uci = tokens[1] || '';
        if (uci === '(none)' || uci === '0000') uci = '';
        // Drop the 5th promotion char; execute_move auto-queens.
        if (uci.length > 4) uci = uci.substring(0, 4);
        mode = null;
        safe_ccall('on_ai_move_from_js', null, ['string'], [uci]);
      }
      return;
    }

    if (mode === 'eval') {
      // Parse "info ... score (cp|mate) N ..." for the latest score.
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
        // Negate if black-to-move so the C++ side gets a white-relative score.
        let cp = bestEval;
        if (sideToMoveBlack) cp = -cp;
        const idx = pendingIdx;
        mode = null;
        safe_ccall('on_eval_from_js', null,
                   ['number', 'number'], [cp, idx]);
      }
      return;
    }
  };

  // UCI handshake. The lite engine accepts UCI_LimitStrength + UCI_Elo.
  worker.postMessage('uci');
  worker.postMessage('setoption name UCI_LimitStrength value true');
  worker.postMessage('setoption name UCI_Elo value 1400');
  worker.postMessage('isready');

  function go(fen, movetime) {
    worker.postMessage('ucinewgame');
    worker.postMessage('position fen ' + fen);
    worker.postMessage('go movetime ' + movetime);
  }

  function fenSideToMove(fen) {
    // FEN second field is 'w' or 'b'.
    const parts = fen.split(' ');
    return parts.length >= 2 && parts[1] === 'b';
  }

  return {
    requestMove: function (fen, movetime) {
      mode = 'move';
      pendingMoveTime = movetime;
      sideToMoveBlack = fenSideToMove(fen);
      go(fen, movetime);
    },
    requestEval: function (fen, movetime, idx) {
      mode = 'eval';
      pendingIdx = idx;
      bestEval = 0;
      sideToMoveBlack = fenSideToMove(fen);
      go(fen, movetime);
    },
  };
})();
