/*
 * 3D Chess — Meta Ray-Ban Display HUD (Web App scaffold, M0).
 *
 * A glanceable chess HUD for the 600x600 in-lens display, driven entirely by
 * the D-pad key events the glasses emit (Neural Band swipes/pinches and frame
 * cap-touch arrive as standard ArrowUp/Down/Left/Right + Enter + Escape
 * `keydown` events — see docs/meta-rayban-display.md).
 *
 * STATUS: scaffold. Runs in a desktop browser (arrow keys = D-pad) exactly as
 * Meta's local-test loop intends. The chess RULES and the AI ENGINE are
 * STUBBED — the two seams where the real implementations plug in are marked
 * `M1 SEAM` below:
 *   - applyMoveStub()      -> real legality (shared C++ rules compiled to WASM,
 *                             or a small JS rules port)
 *   - requestEngineMove()  -> the vendored Stockfish.js worker (web/stockfish/,
 *                             mirroring web/stockfish-bridge.js)
 */

'use strict';

// ---------------------------------------------------------------------------
// Board model
// ---------------------------------------------------------------------------
// board[r][c]: r=0 is rank 8 (top of HUD), c=0 is file a. Uppercase = white,
// lowercase = black, '.' = empty. Standard starting position.
const START = [
  ['r','n','b','q','k','b','n','r'],
  ['p','p','p','p','p','p','p','p'],
  ['.','.','.','.','.','.','.','.'],
  ['.','.','.','.','.','.','.','.'],
  ['.','.','.','.','.','.','.','.'],
  ['.','.','.','.','.','.','.','.'],
  ['P','P','P','P','P','P','P','P'],
  ['R','N','B','Q','K','B','N','R'],
];

const GLYPH = {
  K:'♔', Q:'♕', R:'♖', B:'♗', N:'♘', P:'♙',
  k:'♚', q:'♛', r:'♜', b:'♝', n:'♞', p:'♟',
};

const state = {
  board: START.map(row => row.slice()),
  whiteToMove: true,
  humanIsWhite: true,
  cursor: { r: 6, c: 4 },   // start on the e2 pawn
  picked: null,             // {r,c} once a from-square is chosen
  last: null,               // {src:{r,c}, dst:{r,c}, san, cls}
  evalCp: 0,                // white-relative centipawns (stub)
  whiteMs: 10 * 60 * 1000,
  blackMs: 10 * 60 * 1000,
  thinking: false,
};

// ---------------------------------------------------------------------------
// Coordinate helpers
// ---------------------------------------------------------------------------
const fileChar = c => 'abcdefgh'[c];
const rankChar = r => '87654321'[r];
const sqName   = (r, c) => fileChar(c) + rankChar(r);
const isWhitePiece = p => p !== '.' && p === p.toUpperCase();
const sideOwns = p => p !== '.' && (isWhitePiece(p) === state.whiteToMove);

// FEN of the current position — the exact string the engine seam consumes.
function toFen() {
  let rows = state.board.map(row => {
    let out = '', empty = 0;
    for (const p of row) {
      if (p === '.') { empty++; continue; }
      if (empty) { out += empty; empty = 0; }
      out += p;
    }
    if (empty) out += empty;
    return out;
  }).join('/');
  // Castling/en-passant are omitted in the stub; M1 carries full state.
  return `${rows} ${state.whiteToMove ? 'w' : 'b'} - - 0 1`;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
const el = {
  board:    document.getElementById('board'),
  prompt:   document.getElementById('prompt'),
  clock:    document.getElementById('clock'),
  evalfill: document.getElementById('evalfill'),
  evaltext: document.getElementById('evaltext'),
  lastmove: document.getElementById('lastmove'),
  badge:    document.getElementById('badge'),
};

// Build the 64 cells once; we only toggle classes/text afterwards.
const cells = [];
for (let r = 0; r < 8; r++) {
  cells[r] = [];
  for (let c = 0; c < 8; c++) {
    const div = document.createElement('div');
    div.className = 'sq ' + ((r + c) % 2 === 0 ? 'light' : 'dark');
    div.setAttribute('role', 'gridcell');
    el.board.appendChild(div);
    cells[r][c] = div;
  }
}

function renderBoard() {
  for (let r = 0; r < 8; r++) {
    for (let c = 0; c < 8; c++) {
      const div = cells[r][c];
      div.classList.remove('cursor', 'picked', 'lastsrc', 'lastdst');
      const p = state.board[r][c];
      div.innerHTML = p === '.' ? '' :
        `<span class="pc ${isWhitePiece(p) ? 'white' : 'black'}">${GLYPH[p]}</span>`;
      if (state.last) {
        if (r === state.last.src.r && c === state.last.src.c) div.classList.add('lastsrc');
        if (r === state.last.dst.r && c === state.last.dst.c) div.classList.add('lastdst');
      }
      if (state.picked && r === state.picked.r && c === state.picked.c) div.classList.add('picked');
      if (r === state.cursor.r && c === state.cursor.c) div.classList.add('cursor');
    }
  }
}

function fmtClock(ms) {
  const s = Math.max(0, Math.round(ms / 1000));
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
}

function renderHud() {
  state.prompt = state.thinking ? 'Thinking…'
               : (state.whiteToMove === state.humanIsWhite ? 'Your move' : 'Waiting');
  el.prompt.textContent = state.prompt;
  el.prompt.classList.toggle('waiting', state.prompt !== 'Your move');

  const myMs = state.humanIsWhite ? state.whiteMs : state.blackMs;
  el.clock.textContent = fmtClock(myMs);
  el.clock.classList.toggle('low', myMs < 30000);

  // Eval bar: clamp centipawns to a +/-1000 window, white fills from the left.
  const cp = Math.max(-1000, Math.min(1000, state.evalCp));
  el.evalfill.style.width = `${50 + (cp / 1000) * 50}%`;
  const pawns = (cp / 100).toFixed(1);
  el.evaltext.textContent = cp > 0 ? `+${pawns}` : pawns;

  if (state.last) {
    el.lastmove.textContent = state.last.san;
    el.badge.textContent = state.last.cls ? state.last.cls.glyph : '';
    el.badge.className = 'badge ' + (state.last.cls ? state.last.cls.kind : '');
  } else {
    el.lastmove.textContent = '—';
    el.badge.textContent = '';
    el.badge.className = 'badge';
  }
}

function render() { renderBoard(); renderHud(); }

// ---------------------------------------------------------------------------
// Move application — M1 SEAM (legality is stubbed)
// ---------------------------------------------------------------------------
// STUB: moves the piece on `src` onto `dst` with no legality check. Replace
// with the shared C++ rules compiled to WASM (FEN in / legal-move check /
// SAN out) so the HUD matches the desktop & web builds exactly.
function applyMoveStub(src, dst) {
  const piece = state.board[src.r][src.c];
  if (piece === '.') return false;
  state.board[dst.r][dst.c] = piece;
  state.board[src.r][src.c] = '.';
  state.last = { src, dst, san: sqName(src.r, src.c) + sqName(dst.r, dst.c), cls: null };
  state.whiteToMove = !state.whiteToMove;
  save();
  return true;
}

// ---------------------------------------------------------------------------
// Engine — M1 SEAM (Stockfish.js wiring is stubbed)
// ---------------------------------------------------------------------------
// STUB: instead of posting `fen` to the vendored Stockfish.js worker
// (web/stockfish/, see web/stockfish-bridge.js) and awaiting `bestmove`, this
// echoes a trivial canned reply so the turn-flow is demonstrable. The eval is
// a placeholder. M1 replaces the body; `onEngineMove` stays the callback.
function requestEngineMove(fen) {
  state.thinking = true;
  renderHud();
  setTimeout(() => {
    // Canned opening replies for Black so the demo plays a few moves; falls
    // back to "no reply" once off-book (stub has no move generator).
    const canned = { 1: { from: { r: 1, c: 4 }, to: { r: 3, c: 4 } },   // e7e5
                     2: { from: { r: 0, c: 6 }, to: { r: 2, c: 5 } } };  // g8f6
    const reply = canned[++requestEngineMove._n];
    if (reply) onEngineMove(reply.from, reply.to);
    else { state.thinking = false; renderHud(); }   // off-book: human plays on
  }, 500);
}
requestEngineMove._n = 0;

function onEngineMove(src, dst) {
  applyMoveStub(src, dst);
  state.thinking = false;
  state.evalCp = Math.round((Math.random() * 80) - 40);  // stub eval
  render();
}

// ---------------------------------------------------------------------------
// Move-quality badge vocabulary (matches the desktop/web classifier glyphs).
// In M1 this is fed by the eval swing instead of being hand-set.
// ---------------------------------------------------------------------------
const CLS = {
  brilliant: { glyph: '!!', kind: 'brilliant' },
  good:      { glyph: '!',  kind: 'good' },
  miss:      { glyph: '?!', kind: 'miss' },
  mistake:   { glyph: '?',  kind: 'mistake' },
  blunder:   { glyph: '??', kind: 'blunder' },
};

// ---------------------------------------------------------------------------
// Input — D-pad (Neural Band swipes/pinches => arrow keys + Enter + Escape)
// ---------------------------------------------------------------------------
function moveCursor(dr, dc) {
  state.cursor.r = Math.max(0, Math.min(7, state.cursor.r + dr));
  state.cursor.c = Math.max(0, Math.min(7, state.cursor.c + dc));
  renderBoard();
}

function activate() {
  if (state.thinking) return;
  if (state.whiteToMove !== state.humanIsWhite) return;  // not your turn
  const { r, c } = state.cursor;
  if (!state.picked) {
    // Pick a from-square only if it holds a piece of the side to move.
    if (sideOwns(state.board[r][c])) { state.picked = { r, c }; renderBoard(); }
    return;
  }
  // Second tap: commit the from -> to move.
  const src = state.picked;
  state.picked = null;
  if (src.r === r && src.c === c) { renderBoard(); return; }  // tapped same sq = cancel
  if (applyMoveStub(src, { r, c })) {
    render();
    requestEngineMove(toFen());   // ask the engine for the reply
  } else {
    render();
  }
}

document.addEventListener('keydown', (e) => {
  switch (e.key) {
    case 'ArrowUp':    moveCursor(-1, 0); break;
    case 'ArrowDown':  moveCursor(1, 0);  break;
    case 'ArrowLeft':  moveCursor(0, -1); break;
    case 'ArrowRight': moveCursor(0, 1);  break;
    case 'Enter':      activate();        break;
    case 'Escape':     state.picked = null; renderBoard(); break;
    default: return;
  }
  e.preventDefault();
});

// ---------------------------------------------------------------------------
// Clock + persistence
// ---------------------------------------------------------------------------
let lastTick = performance.now();
function tick(now) {
  const dt = now - lastTick;
  lastTick = now;
  if (!state.thinking || state.whiteToMove !== state.humanIsWhite) {
    if (state.whiteToMove) state.whiteMs -= dt; else state.blackMs -= dt;
  }
  el.clock.textContent = fmtClock(state.humanIsWhite ? state.whiteMs : state.blackMs);
  requestAnimationFrame(tick);
}

function save() {
  try {
    localStorage.setItem('glasses_chess', JSON.stringify({
      board: state.board, whiteToMove: state.whiteToMove, last: state.last,
      whiteMs: state.whiteMs, blackMs: state.blackMs,
    }));
  } catch (_) { /* localStorage may be unavailable; non-fatal */ }
}

function restore() {
  try {
    const raw = localStorage.getItem('glasses_chess');
    if (!raw) return;
    const s = JSON.parse(raw);
    Object.assign(state, {
      board: s.board, whiteToMove: s.whiteToMove, last: s.last,
      whiteMs: s.whiteMs, blackMs: s.blackMs,
    });
  } catch (_) { /* ignore corrupt save */ }
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------
restore();
render();
requestAnimationFrame((t) => { lastTick = t; tick(t); });
