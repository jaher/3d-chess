/*
 * 3D Chess — Meta Ray-Ban Display Web App: AI engine.
 *
 * A self-contained negamax + alpha-beta search with iterative deepening, a wall-
 * clock time budget, MVV-LVA move ordering, and a quiescence search, over a
 * material + piece-square-table evaluation. Pure logic, no DOM; attaches to
 * globalThis.ChessEngine and depends on globalThis.ChessRules (loaded first via
 * <script> in the page, or importScripts() in the worker).
 *
 * Stockfish.js (used by the desktop/full web build) is intentionally NOT used
 * here: the glasses run a lightweight on-device browser and the engine must be
 * a small, dependency-free bundle. This search lands around club strength and
 * replies within its time budget, which is the right fit for a glanceable HUD.
 */
(function (global) {
  'use strict';

  const R = global.ChessRules;
  const MATE = 1000000;

  const VAL = { P: 100, N: 320, B: 330, R: 500, Q: 900, K: 0 };

  // Piece-square tables in this project's coords: PST[r][c], r=0 = rank 8,
  // r=7 = rank 1, given from WHITE's perspective. Black reads PST[7-r][c].
  const PST = {
    P: [
      [0, 0, 0, 0, 0, 0, 0, 0],
      [50, 50, 50, 50, 50, 50, 50, 50],
      [10, 10, 20, 30, 30, 20, 10, 10],
      [5, 5, 10, 25, 25, 10, 5, 5],
      [0, 0, 0, 20, 20, 0, 0, 0],
      [5, -5, -10, 0, 0, -10, -5, 5],
      [5, 10, 10, -20, -20, 10, 10, 5],
      [0, 0, 0, 0, 0, 0, 0, 0],
    ],
    N: [
      [-50, -40, -30, -30, -30, -30, -40, -50],
      [-40, -20, 0, 0, 0, 0, -20, -40],
      [-30, 0, 10, 15, 15, 10, 0, -30],
      [-30, 5, 15, 20, 20, 15, 5, -30],
      [-30, 0, 15, 20, 20, 15, 0, -30],
      [-30, 5, 10, 15, 15, 10, 5, -30],
      [-40, -20, 0, 5, 5, 0, -20, -40],
      [-50, -40, -30, -30, -30, -30, -40, -50],
    ],
    B: [
      [-20, -10, -10, -10, -10, -10, -10, -20],
      [-10, 0, 0, 0, 0, 0, 0, -10],
      [-10, 0, 5, 10, 10, 5, 0, -10],
      [-10, 5, 5, 10, 10, 5, 5, -10],
      [-10, 0, 10, 10, 10, 10, 0, -10],
      [-10, 10, 10, 10, 10, 10, 10, -10],
      [-10, 5, 0, 0, 0, 0, 5, -10],
      [-20, -10, -10, -10, -10, -10, -10, -20],
    ],
    R: [
      [0, 0, 0, 0, 0, 0, 0, 0],
      [5, 10, 10, 10, 10, 10, 10, 5],
      [-5, 0, 0, 0, 0, 0, 0, -5],
      [-5, 0, 0, 0, 0, 0, 0, -5],
      [-5, 0, 0, 0, 0, 0, 0, -5],
      [-5, 0, 0, 0, 0, 0, 0, -5],
      [-5, 0, 0, 0, 0, 0, 0, -5],
      [0, 0, 0, 5, 5, 0, 0, 0],
    ],
    Q: [
      [-20, -10, -10, -5, -5, -10, -10, -20],
      [-10, 0, 0, 0, 0, 0, 0, -10],
      [-10, 0, 5, 5, 5, 5, 0, -10],
      [-5, 0, 5, 5, 5, 5, 0, -5],
      [0, 0, 5, 5, 5, 5, 0, -5],
      [-10, 5, 5, 5, 5, 5, 0, -10],
      [-10, 0, 5, 0, 0, 0, 0, -10],
      [-20, -10, -10, -5, -5, -10, -10, -20],
    ],
    K: [
      [-30, -40, -40, -50, -50, -40, -40, -30],
      [-30, -40, -40, -50, -50, -40, -40, -30],
      [-30, -40, -40, -50, -50, -40, -40, -30],
      [-30, -40, -40, -50, -50, -40, -40, -30],
      [-20, -30, -30, -40, -40, -30, -30, -20],
      [-10, -20, -20, -20, -20, -20, -20, -10],
      [20, 20, 0, 0, 0, 0, 20, 20],
      [20, 30, 10, 0, 0, 10, 30, 20],
    ],
  };

  // Static evaluation, WHITE-relative centipawns (positive = white is better).
  function evaluate(s) {
    let score = 0;
    const b = s.board;
    for (let r = 0; r < 8; r++) {
      for (let c = 0; c < 8; c++) {
        const p = b[r][c];
        if (p === '.') continue;
        const u = p.toUpperCase();
        const white = R.isWhite(p);
        const mat = VAL[u];
        const pst = white ? PST[u][r][c] : PST[u][7 - r][c];
        score += white ? (mat + pst) : -(mat + pst);
      }
    }
    return score;
  }

  // Order moves to make alpha-beta cut earlier: promotions and high MVV-LVA
  // captures first, quiet moves last.
  function scoreMove(m) {
    let s = 0;
    if (m.promo) s += 900;
    if (m.capture) s += 10 * VAL[m.capture.toUpperCase()] - VAL[m.piece.toUpperCase()] + 1000;
    if (m.castle) s += 50;
    return s;
  }
  function order(moves) {
    return moves.map((m) => [scoreMove(m), m]).sort((a, b) => b[0] - a[0]).map((x) => x[1]);
  }

  // Quiescence: only search captures/promotions so the eval is taken at a
  // "quiet" position (avoids the horizon effect on hanging pieces).
  function quiesce(s, alpha, beta, ctx) {
    ctx.nodes++;
    const standRel = (s.white ? 1 : -1) * evaluate(s);
    if (standRel >= beta) return beta;
    if (standRel > alpha) alpha = standRel;
    const caps = order(R.legalMoves(s).filter((m) => m.capture || m.promo));
    for (const m of caps) {
      const score = -quiesce(R.applyMove(s, m), -beta, -alpha, ctx);
      if (score >= beta) return beta;
      if (score > alpha) alpha = score;
    }
    return alpha;
  }

  function negamax(s, depth, alpha, beta, ply, ctx) {
    if (ctx.stop || (++ctx.nodes & 1023) === 0 && Date.now() > ctx.deadline) { ctx.stop = true; return 0; }
    const moves = R.legalMoves(s);
    if (moves.length === 0) {
      return R.inCheck(s, s.white) ? -(MATE - ply) : 0;   // mate (deeper = worse) or stalemate
    }
    if (depth === 0) return quiesce(s, alpha, beta, ctx);
    let best = -Infinity;
    let bestMove = null;
    for (const m of order(moves)) {
      const score = -negamax(R.applyMove(s, m), depth - 1, -beta, -alpha, ply + 1, ctx);
      if (ctx.stop) return best === -Infinity ? 0 : best;
      if (score > best) { best = score; bestMove = m; }
      if (score > alpha) alpha = score;
      if (alpha >= beta) break;
    }
    if (ply === 0) ctx.rootBest = bestMove;
    return best;
  }

  // Search the best move for the side to move in `state`.
  // opts: { timeMs (budget), maxDepth }. Returns { move, evalCp, depth, nodes }.
  function searchBestMove(state, opts) {
    opts = opts || {};
    const timeMs = opts.timeMs || 600;
    const maxDepth = opts.maxDepth || 6;
    const moves = R.legalMoves(state);
    if (moves.length === 0) return { move: null, evalCp: 0, depth: 0, nodes: 0 };

    const ctx = { deadline: Date.now() + timeMs, nodes: 0, stop: false, rootBest: null };
    let best = order(moves)[0];           // safe fallback if we never finish depth 1
    let bestScore = 0, reached = 0;
    for (let depth = 1; depth <= maxDepth; depth++) {
      ctx.rootBest = null;
      const score = negamax(state, depth, -Infinity, Infinity, 0, ctx);
      if (ctx.stop) break;                // ran out of time mid-depth; keep prior result
      if (ctx.rootBest) { best = ctx.rootBest; bestScore = score; reached = depth; }
      if (Math.abs(score) > MATE - 100) break;   // forced mate found — stop early
      if (Date.now() > ctx.deadline) break;
    }
    const evalCp = (state.white ? 1 : -1) * bestScore;   // convert to white-relative
    return { move: best, evalCp, depth: reached, nodes: ctx.nodes };
  }

  global.ChessEngine = { searchBestMove, evaluate };

  // `node engine.js` plays a couple of self-moves as a smoke test.
  if (typeof process !== 'undefined' && process.argv && process.argv[2] === 'smoke') {
    let s = R.initialState();
    for (let i = 0; i < 6; i++) {
      const r = searchBestMove(s, { timeMs: 300 });
      if (!r.move) { console.log('no move (', R.result(s), ')'); break; }
      console.log(i, R.toSan(s, r.move), 'd' + r.depth, 'eval', r.evalCp, 'nodes', r.nodes);
      s = R.applyMove(s, r.move);
    }
  }
})(typeof self !== 'undefined' ? self : (typeof globalThis !== 'undefined' ? globalThis : this));
