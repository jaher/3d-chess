/*
 * 3D Chess — Meta Ray-Ban Display Web App: chess rules engine.
 *
 * Pure logic, no DOM. Attaches to globalThis.ChessRules so it works both in
 * the page (via <script>) and in the engine Web Worker (via importScripts).
 *
 * Board model: board[r][c], r=0 is rank 8 (top of the HUD), c=0 is file a.
 * Pieces are single chars: KQRBNP = white, kqrbnp = black, '.' = empty.
 * This matches glasses/app.js and mirrors the desktop/web engine's geometry.
 *
 * Correctness is pinned by perft (see selfTest() / `node chess.js perft`):
 *   depth 1 = 20, 2 = 400, 3 = 8902, 4 = 197281, 5 = 4865609.
 */
(function (global) {
  'use strict';

  const WHITE_PIECES = 'KQRBNP';
  const isWhite = (p) => p !== '.' && p >= 'A' && p <= 'Z';
  const isBlack = (p) => p !== '.' && p >= 'a' && p <= 'z';
  const inBounds = (r, c) => r >= 0 && r < 8 && c >= 0 && c < 8;
  const colorOf = (p) => (p === '.' ? null : (isWhite(p) ? 'w' : 'b'));

  function initialState() {
    return {
      board: [
        ['r', 'n', 'b', 'q', 'k', 'b', 'n', 'r'],
        ['p', 'p', 'p', 'p', 'p', 'p', 'p', 'p'],
        ['.', '.', '.', '.', '.', '.', '.', '.'],
        ['.', '.', '.', '.', '.', '.', '.', '.'],
        ['.', '.', '.', '.', '.', '.', '.', '.'],
        ['.', '.', '.', '.', '.', '.', '.', '.'],
        ['P', 'P', 'P', 'P', 'P', 'P', 'P', 'P'],
        ['R', 'N', 'B', 'Q', 'K', 'B', 'N', 'R'],
      ],
      white: true,                                   // white to move
      castling: { wK: true, wQ: true, bK: true, bQ: true },
      ep: null,                                       // {r,c} en-passant target square, or null
      half: 0,                                        // halfmove clock (50-move rule)
      full: 1,                                        // fullmove number
    };
  }

  function cloneState(s) {
    return {
      board: s.board.map((row) => row.slice()),
      white: s.white,
      castling: { wK: s.castling.wK, wQ: s.castling.wQ, bK: s.castling.bK, bQ: s.castling.bQ },
      ep: s.ep ? { r: s.ep.r, c: s.ep.c } : null,
      half: s.half,
      full: s.full,
    };
  }

  // Ray directions for sliding pieces.
  const DIAG = [[-1, -1], [-1, 1], [1, -1], [1, 1]];
  const ORTHO = [[-1, 0], [1, 0], [0, -1], [0, 1]];
  const KNIGHT = [[-2, -1], [-2, 1], [-1, -2], [-1, 2], [1, -2], [1, 2], [2, -1], [2, 1]];
  const KING = DIAG.concat(ORTHO);

  // Is square (r,c) attacked by the side `byWhite`?
  function isAttacked(board, r, c, byWhite) {
    // Pawns: a white pawn attacks the two squares diagonally "up" (toward r-1).
    const pdir = byWhite ? 1 : -1;   // the rank offset FROM the target TO the attacking pawn
    for (const dc of [-1, 1]) {
      const pr = r + pdir, pc = c + dc;
      if (inBounds(pr, pc)) {
        const p = board[pr][pc];
        if (p === (byWhite ? 'P' : 'p')) return true;
      }
    }
    // Knights.
    for (const [dr, dc] of KNIGHT) {
      const nr = r + dr, nc = c + dc;
      if (inBounds(nr, nc) && board[nr][nc] === (byWhite ? 'N' : 'n')) return true;
    }
    // King.
    for (const [dr, dc] of KING) {
      const nr = r + dr, nc = c + dc;
      if (inBounds(nr, nc) && board[nr][nc] === (byWhite ? 'K' : 'k')) return true;
    }
    // Sliding: bishops/queens on diagonals, rooks/queens orthogonally.
    for (const [dr, dc] of DIAG) {
      let nr = r + dr, nc = c + dc;
      while (inBounds(nr, nc)) {
        const p = board[nr][nc];
        if (p !== '.') {
          if (colorOf(p) === (byWhite ? 'w' : 'b') && (p === 'B' || p === 'b' || p === 'Q' || p === 'q')) return true;
          break;
        }
        nr += dr; nc += dc;
      }
    }
    for (const [dr, dc] of ORTHO) {
      let nr = r + dr, nc = c + dc;
      while (inBounds(nr, nc)) {
        const p = board[nr][nc];
        if (p !== '.') {
          if (colorOf(p) === (byWhite ? 'w' : 'b') && (p === 'R' || p === 'r' || p === 'Q' || p === 'q')) return true;
          break;
        }
        nr += dr; nc += dc;
      }
    }
    return false;
  }

  function findKing(board, white) {
    const k = white ? 'K' : 'k';
    for (let r = 0; r < 8; r++) for (let c = 0; c < 8; c++) if (board[r][c] === k) return { r, c };
    return null;
  }

  function inCheck(s, white) {
    const k = findKing(s.board, white);
    return k ? isAttacked(s.board, k.r, k.c, !white) : false;
  }

  // Pseudo-legal moves for the side to move (king-safety filtered later).
  // Move shape: {from:{r,c}, to:{r,c}, piece, capture, promo, castle, ep, dbl}
  function pseudoMoves(s) {
    const out = [];
    const b = s.board;
    const white = s.white;
    const own = white ? isWhite : isBlack;
    const enemy = white ? isBlack : isWhite;
    const push = (from, to, extra) => out.push(Object.assign({
      from, to, piece: b[from.r][from.c], capture: b[to.r][to.c] === '.' ? null : b[to.r][to.c],
      promo: null, castle: null, ep: false, dbl: false,
    }, extra || {}));

    for (let r = 0; r < 8; r++) {
      for (let c = 0; c < 8; c++) {
        const p = b[r][c];
        if (p === '.' || !own(p)) continue;
        const up = p.toUpperCase();
        if (up === 'P') {
          const dir = white ? -1 : 1;            // white pawns move toward r=0
          const startRank = white ? 6 : 1;
          const promoRank = white ? 0 : 7;
          const one = r + dir;
          // Single + double push.
          if (inBounds(one, c) && b[one][c] === '.') {
            if (one === promoRank) for (const pr of ['Q', 'R', 'B', 'N']) push({ r, c }, { r: one, c }, { promo: white ? pr : pr.toLowerCase() });
            else push({ r, c }, { r: one, c });
            const two = r + 2 * dir;
            if (r === startRank && b[two][c] === '.') push({ r, c }, { r: two, c }, { dbl: true });
          }
          // Captures (incl. promotion captures) + en passant.
          for (const dc of [-1, 1]) {
            const tr = r + dir, tc = c + dc;
            if (!inBounds(tr, tc)) continue;
            const t = b[tr][tc];
            if (t !== '.' && enemy(t)) {
              if (tr === promoRank) for (const pr of ['Q', 'R', 'B', 'N']) push({ r, c }, { r: tr, c: tc }, { promo: white ? pr : pr.toLowerCase() });
              else push({ r, c }, { r: tr, c: tc });
            } else if (s.ep && s.ep.r === tr && s.ep.c === tc) {
              push({ r, c }, { r: tr, c: tc }, { ep: true, capture: white ? 'p' : 'P' });
            }
          }
        } else if (up === 'N') {
          for (const [dr, dc] of KNIGHT) {
            const tr = r + dr, tc = c + dc;
            if (inBounds(tr, tc) && !own(b[tr][tc])) push({ r, c }, { r: tr, c: tc });
          }
        } else if (up === 'K') {
          for (const [dr, dc] of KING) {
            const tr = r + dr, tc = c + dc;
            if (inBounds(tr, tc) && !own(b[tr][tc])) push({ r, c }, { r: tr, c: tc });
          }
          // Castling: king on its home square, rights set, squares empty, king
          // not in/through check. Home row is r=7 (white) / r=0 (black).
          const hr = white ? 7 : 0;
          if (r === hr && c === 4 && !isAttacked(b, hr, 4, !white)) {
            const kSide = white ? s.castling.wK : s.castling.bK;
            const qSide = white ? s.castling.wQ : s.castling.bQ;
            if (kSide && b[hr][5] === '.' && b[hr][6] === '.' &&
                b[hr][7] === (white ? 'R' : 'r') &&
                !isAttacked(b, hr, 5, !white) && !isAttacked(b, hr, 6, !white)) {
              push({ r: hr, c: 4 }, { r: hr, c: 6 }, { castle: 'K' });
            }
            if (qSide && b[hr][3] === '.' && b[hr][2] === '.' && b[hr][1] === '.' &&
                b[hr][0] === (white ? 'R' : 'r') &&
                !isAttacked(b, hr, 3, !white) && !isAttacked(b, hr, 2, !white)) {
              push({ r: hr, c: 4 }, { r: hr, c: 2 }, { castle: 'Q' });
            }
          }
        } else {
          // Sliding pieces.
          const dirs = up === 'B' ? DIAG : up === 'R' ? ORTHO : KING;  // Q uses KING (all 8)
          for (const [dr, dc] of dirs) {
            let tr = r + dr, tc = c + dc;
            while (inBounds(tr, tc)) {
              const t = b[tr][tc];
              if (t === '.') { push({ r, c }, { r: tr, c: tc }); }
              else { if (enemy(t)) push({ r, c }, { r: tr, c: tc }); break; }
              tr += dr; tc += dc;
            }
          }
        }
      }
    }
    return out;
  }

  // Apply a (legal or pseudo-legal) move, returning a NEW state.
  function applyMove(s, m) {
    const ns = cloneState(s);
    const b = ns.board;
    const piece = b[m.from.r][m.from.c];
    const white = s.white;

    // Move the piece.
    b[m.to.r][m.to.c] = m.promo ? m.promo : piece;
    b[m.from.r][m.from.c] = '.';

    // En-passant capture removes the pawn that just double-pushed (behind dst).
    if (m.ep) b[m.from.r][m.to.c] = '.';

    // Castling: shuttle the rook.
    if (m.castle === 'K') { b[m.from.r][5] = b[m.from.r][7]; b[m.from.r][7] = '.'; }
    if (m.castle === 'Q') { b[m.from.r][3] = b[m.from.r][0]; b[m.from.r][0] = '.'; }

    // Castling-rights updates: king move, rook move off home, rook captured.
    if (piece === 'K') { ns.castling.wK = ns.castling.wQ = false; }
    if (piece === 'k') { ns.castling.bK = ns.castling.bQ = false; }
    const touch = (r, c) => {
      if (r === 7 && c === 0) ns.castling.wQ = false;
      if (r === 7 && c === 7) ns.castling.wK = false;
      if (r === 0 && c === 0) ns.castling.bQ = false;
      if (r === 0 && c === 7) ns.castling.bK = false;
    };
    touch(m.from.r, m.from.c);   // rook leaving a corner
    touch(m.to.r, m.to.c);       // rook captured on a corner

    // En-passant target: only after a double pawn push.
    ns.ep = m.dbl ? { r: (m.from.r + m.to.r) / 2, c: m.from.c } : null;

    // Clocks.
    const isPawn = piece === 'P' || piece === 'p';
    ns.half = (isPawn || m.capture) ? 0 : s.half + 1;
    if (!white) ns.full = s.full + 1;
    ns.white = !white;
    return ns;
  }

  function legalMoves(s) {
    const mover = s.white;
    return pseudoMoves(s).filter((m) => !inCheck(applyMove(s, m), mover));
  }

  function insufficientMaterial(board) {
    const minors = [];
    for (let r = 0; r < 8; r++) for (let c = 0; c < 8; c++) {
      const p = board[r][c]; if (p === '.') continue;
      const u = p.toUpperCase();
      if (u === 'Q' || u === 'R' || u === 'P') return false;
      if (u === 'B' || u === 'N') minors.push({ u, sq: (r + c) % 2 });
    }
    if (minors.length === 0) return true;                    // K vs K
    if (minors.length === 1) return true;                    // K+minor vs K
    if (minors.length === 2 && minors.every((m) => m.u === 'B') && minors[0].sq === minors[1].sq) return true; // KB vs KB same color
    return false;
  }

  // Returns one of: 'checkmate','stalemate','fifty','insufficient', or null.
  function result(s) {
    if (legalMoves(s).length === 0) return inCheck(s, s.white) ? 'checkmate' : 'stalemate';
    if (s.half >= 100) return 'fifty';
    if (insufficientMaterial(s.board)) return 'insufficient';
    return null;
  }

  // Standard Algebraic Notation for a move in position `s` (before the move).
  function toSan(s, m) {
    if (m.castle === 'K') return suffix(s, m, 'O-O');
    if (m.castle === 'Q') return suffix(s, m, 'O-O-O');
    const piece = m.piece.toUpperCase();
    const file = 'abcdefgh', rank = '87654321';
    const dst = file[m.to.c] + rank[m.to.r];
    let san;
    if (piece === 'P') {
      san = (m.capture ? file[m.from.c] + 'x' : '') + dst + (m.promo ? '=' + m.promo.toUpperCase() : '');
    } else {
      // Disambiguation: other same-type pieces that can also reach dst.
      const peers = legalMoves(s).filter((x) =>
        x.piece === m.piece && x.to.r === m.to.r && x.to.c === m.to.c &&
        !(x.from.r === m.from.r && x.from.c === m.from.c));
      let dis = '';
      if (peers.length) {
        const sameFile = peers.some((x) => x.from.c === m.from.c);
        const sameRank = peers.some((x) => x.from.r === m.from.r);
        if (!sameFile) dis = file[m.from.c];
        else if (!sameRank) dis = rank[m.from.r];
        else dis = file[m.from.c] + rank[m.from.r];
      }
      san = piece + dis + (m.capture ? 'x' : '') + dst;
    }
    return suffix(s, m, san);
  }
  function suffix(s, m, san) {
    const ns = applyMove(s, m);
    if (inCheck(ns, ns.white)) san += (legalMoves(ns).length === 0 ? '#' : '+');
    return san;
  }

  function toFen(s) {
    const rows = s.board.map((row) => {
      let out = '', empty = 0;
      for (const p of row) {
        if (p === '.') { empty++; continue; }
        if (empty) { out += empty; empty = 0; }
        out += p;
      }
      if (empty) out += empty;
      return out;
    }).join('/');
    let cr = (s.castling.wK ? 'K' : '') + (s.castling.wQ ? 'Q' : '') +
             (s.castling.bK ? 'k' : '') + (s.castling.bQ ? 'q' : '');
    if (!cr) cr = '-';
    let ep = '-';
    if (s.ep) ep = 'abcdefgh'[s.ep.c] + '87654321'[s.ep.r];
    return `${rows} ${s.white ? 'w' : 'b'} ${cr} ${ep} ${s.half} ${s.full}`;
  }

  // --- perft: the move-generator correctness oracle ------------------------
  function perft(s, depth) {
    if (depth === 0) return 1;
    let n = 0;
    for (const m of legalMoves(s)) n += perft(applyMove(s, m), depth - 1);
    return n;
  }

  function selfTest() {
    const want = [1, 20, 400, 8902, 197281];
    const s = initialState();
    const got = want.map((_, d) => perft(s, d));
    const ok = got.every((g, i) => g === want[i]);
    return { ok, got, want };
  }

  global.ChessRules = {
    initialState, cloneState, legalMoves, pseudoMoves, applyMove,
    inCheck, result, insufficientMaterial, toSan, toFen, perft, selfTest,
    isWhite, isBlack, colorOf, inBounds,
  };

  // `node chess.js perft` runs the self-test from the CLI.
  if (typeof process !== 'undefined' && process.argv && process.argv[2] === 'perft') {
    const t = selfTest();
    // eslint-disable-next-line no-console
    console.log('perft', t.ok ? 'PASS' : 'FAIL', 'got=', t.got.join(','), 'want=', t.want.join(','));
    process.exit(t.ok ? 0 : 1);
  }
})(typeof self !== 'undefined' ? self : (typeof globalThis !== 'undefined' ? globalThis : this));
