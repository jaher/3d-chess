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
  // stockfish/stockfish.wasm is 7+ MB — bigger than the rest of the
  // bundle combined — and it's only needed in MODE_PLAYING (the menu,
  // pregame, challenges, analysis-only replay don't touch it). We
  // defer constructing the Worker (and therefore the wasm fetch)
  // until the first requestMove/requestEval call. setElo before that
  // point gets latched into pendingElo and applied during the
  // handshake so the first search uses the right strength.
  let worker = null;

  // Each queued entry: { kind: 'move'|'eval', fen, movetime, idx, game_id }
  const queue = [];
  let active = null;          // currently-running entry, or null
  let bestEval = 0;           // running best eval score for the active eval search
  let handshakeDone = false;  // becomes true once 'readyok' arrives
  let pendingElo = 1400;      // latched until the worker exists

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
    // First real search kicks off the worker fetch. After this the
    // wasm is cached by the browser, so subsequent searches start
    // instantly.
    ensureWorker();
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

  function onWorkerMessage(e) {
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
        const gameId = active.game_id | 0;
        finishActive(function () {
          safe_ccall('on_ai_move_from_js', null,
                     ['string', 'number'], [uci, gameId]);
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
        // Capture the bestmove UCI alongside the cp score — drives
        // the hint feature on the C++ side. Format: "bestmove e7e5
        // ponder d2d4" / "bestmove (none)" / "bestmove 0000".
        const parts = line.split(/\s+/);
        let bestUci = parts.length >= 2 ? parts[1] : '';
        if (bestUci === '(none)' || bestUci === '0000') bestUci = '';
        const gameId = active.game_id | 0;
        finishActive(function () {
          safe_ccall('on_eval_from_js', null,
                     ['number', 'number', 'string', 'number'],
                     [cp, idx, bestUci, gameId]);
        });
      }
      return;
    }
  }

  function ensureWorker() {
    if (worker) return;
    worker = new Worker('stockfish/stockfish.js');
    worker.onmessage = onWorkerMessage;
    // UCI handshake — fire-and-forget. Any queued requests will start
    // streaming after handshakeDone flips; the worker processes
    // messages in order regardless.
    worker.postMessage('uci');
    applyEloInternal(pendingElo);
    worker.postMessage('isready');
  }

  // Apply the right Stockfish strength knob for a given requested
  // ELO. UCI_Elo's documented floor is 1320; below that we use the
  // Skill Level option (0..20) which reaches ~800 ELO at level 0.
  // Must match ai_player.cpp::apply_elo on the desktop path so both
  // builds feel identical at the same slider position.
  function applyEloInternal(elo) {
    elo = elo | 0;
    if (elo >= 1320) {
      // UCI_LimitStrength=true makes Stockfish derive skill level
      // from UCI_Elo internally, overriding any explicit Skill Level
      // that was set on the low-path. Don't re-send Skill Level here
      // — some engine builds latch the explicit value and regressed
      // default-mode strength when we did.
      elo = Math.min(3190, elo);
      worker.postMessage('setoption name UCI_LimitStrength value true');
      worker.postMessage('setoption name UCI_Elo value ' + elo);
    } else {
      if (elo < 800) elo = 800;
      // elo ∈ [800, 1320) → skill ∈ [0, 12].
      var skill = Math.floor(((elo - 800) * 12) / (1320 - 800));
      if (skill < 0) skill = 0;
      if (skill > 19) skill = 19;
      worker.postMessage('setoption name UCI_LimitStrength value false');
      worker.postMessage('setoption name Skill Level value ' + skill);
    }
  }

  // Public setElo: safe to call before the worker exists (latches to
  // pendingElo). Once the worker has been started, apply the value
  // immediately so the next search uses it.
  function applyElo(elo) {
    pendingElo = elo | 0;
    if (worker) applyEloInternal(pendingElo);
  }

  return {
    requestMove: function (fen, movetime, game_id) {
      queue.push({ kind: 'move', fen: fen, movetime: movetime, idx: -1,
                   game_id: game_id | 0 });
      startNext();
    },
    requestEval: function (fen, movetime, idx, game_id) {
      queue.push({ kind: 'eval', fen: fen, movetime: movetime, idx: idx,
                   game_id: game_id | 0 });
      startNext();
    },
    setElo: applyElo,
    // Start loading stockfish.wasm in the background. Intended to be
    // called once the main menu is visible so the 7 MB fetch overlaps
    // with the user reading the menu / navigating pregame rather than
    // stalling the first AI move. Idempotent; no-op if already loaded.
    prefetch: ensureWorker,
  };
})();
