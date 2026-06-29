/*
 * 3D Chess — Meta Ray-Ban Display Web App (full integration).
 *
 * A complete, playable chess game for the 600x600 in-lens display, driven
 * entirely by the D-pad events the glasses emit (Neural Band swipes/pinches and
 * frame cap-touch arrive as standard ArrowUp/Down/Left/Right + Enter + Escape
 * `keydown` events — see docs/meta-rayban-display.md).
 *
 * Real rules come from chess.js (ChessRules, perft-verified) and a real
 * alpha-beta AI from engine.js (ChessEngine), run off-thread in
 * engine-worker.js so the HUD and clock never stall while the AI thinks. No
 * stubs: legal move generation, castling, en passant, promotion, check /
 * checkmate / stalemate / draws, a move-quality coach, a clock with flagging,
 * a settings menu, and localStorage persistence.
 *
 * It runs in any desktop browser too (arrow keys = D-pad), which is exactly
 * Meta's "build and preview in your browser, then deploy via URL" loop.
 */
'use strict';

const R = window.ChessRules;
const E = window.ChessEngine;

const GLYPH = {
  K: '♔', Q: '♕', R: '♖', B: '♗', N: '♘', P: '♙',
  k: '♚', q: '♛', r: '♜', b: '♝', n: '♞', p: '♟',
};

const DIFFS = [
  { name: 'Easy', timeMs: 200, maxDepth: 2 },
  { name: 'Normal', timeMs: 600, maxDepth: 4 },
  { name: 'Hard', timeMs: 1200, maxDepth: 6 },
];

const CLS = {
  good: { glyph: '!', kind: 'good' },
  inaccuracy: { glyph: '?!', kind: 'miss' },
  mistake: { glyph: '?', kind: 'mistake' },
  blunder: { glyph: '??', kind: 'blunder' },
};

// ---------------------------------------------------------------------------
// Move sync (glasses <-> desktop). The glasses Web App has no Web Bluetooth, so
// the link is a WebSocket to the sync-server.js relay; the desktop joins the
// same room over raw TCP. When `linked`, the AI is off and the OTHER colour's
// moves arrive from the remote. See sync.js / sync-server.js.
// ---------------------------------------------------------------------------
const FILE = 'abcdefgh', RANK = '87654321';
function moveToUci(m) {
  return FILE[m.from.c] + RANK[m.from.r] + FILE[m.to.c] + RANK[m.to.r] + (m.promo ? m.promo.toLowerCase() : '');
}
function uciToMove(g, uci) {
  const fc = FILE.indexOf(uci[0]), fr = RANK.indexOf(uci[1]);
  const tc = FILE.indexOf(uci[2]), tr = RANK.indexOf(uci[3]);
  const promo = uci[4] ? uci[4].toUpperCase() : null;
  return R.legalMoves(g).find((m) =>
    m.from.r === fr && m.from.c === fc && m.to.r === tr && m.to.c === tc &&
    (promo ? (m.promo && m.promo.toUpperCase() === promo) : !m.promo));
}

let linked = false;        // a synced game is active
let peerPresent = false;   // the other end is in the room
const sync = {
  status: 'off',           // off | connected | closed | error
  url: () => {
    const proto = (location && location.protocol === 'https:') ? 'wss' : 'ws';
    const host = (location && location.hostname) ? location.hostname : 'localhost';
    return `${proto}://${host}:8090`;
  },
};

function connectSync() {
  if (!window.ChessSync) return;
  sync.status = 'connecting';
  window.ChessSync.connect(sync.url(), String(prefs.room), 'glasses', {
    onMove: applyRemoteMove,
    onReset: onRemoteReset,
    // The relay assigns a role on join: the first into the room is the colour
    // authority (initiator) and sends the reset; a later joiner waits for it.
    onRole: (initiator) => {
      linked = true;
      if (initiator) startLinkedGame(true);
      else { newGame(); render(); }   // follower: wait for the initiator's reset
    },
    onPeer: (name, joined) => {
      peerPresent = joined;
      // If we're the authority and a peer (re)joins, re-send our reset so they sync.
      if (joined && linked && state && state.game.full === 1 && state.game.white) window.ChessSync.sendReset(prefs.humanIsWhite);
      renderHud();
    },
    onStatus: (s) => {
      sync.status = s;
      if (s === 'closed' || s === 'error') { linked = false; peerPresent = false; }
      renderHud();
    },
  });
}
function disconnectSync() {
  if (window.ChessSync) window.ChessSync.disconnect();
  linked = false; peerPresent = false; sync.status = 'off';
}
// Begin a fresh synced game. The initiator is the colour authority: it sends a
// reset carrying its own colour; the remote adopts the opposite.
function startLinkedGame(initiate) {
  linked = true;
  newGame();
  if (initiate && window.ChessSync) window.ChessSync.sendReset(prefs.humanIsWhite);
  render();
}
function onRemoteReset(fromWhite) {
  prefs.humanIsWhite = !fromWhite;   // take the opposite colour to the initiator
  savePrefs();
  linked = true;
  newGame();
  render();
}
function applyRemoteMove(uci) {
  if (!state || !linked) return;
  const g = state.game;
  if (g.white === prefs.humanIsWhite) return;     // not the remote's turn — ignore
  const move = uciToMove(g, uci);
  if (!move) return;                              // unknown/illegal here — ignore (desync guard)
  const san = R.toSan(g, move);
  state.game = R.applyMove(g, move);
  state.last = { from: move.from, to: move.to, san };
  state.badge = null;
  state.evalWhite = E.evaluate(state.game);       // static eval for the bar (no engine when linked)
  state.thinking = false;
  save();
  checkOver();
  render();
}

// Persistent preferences (survive new games + reloads).
let prefs = { humanIsWhite: true, difficulty: 1, room: 1 };
let state = null;
let mode = 'play';        // 'play' | 'promo' | 'menu' | 'over'
let overlay = null;       // { items, idx, ... } for promo/menu/over

function newGame() {
  state = {
    game: R.initialState(),
    cursor: prefs.humanIsWhite ? { r: 6, c: 4 } : { r: 1, c: 4 },
    picked: null,
    targets: [],          // legal moves from the picked square
    last: null,           // { from, to, san }
    badge: null,          // CLS entry for the human's last move
    evalWhite: 0,         // white-relative centipawns (from the engine)
    prevEvalWhite: 0,     // eval before the human's pending move
    whiteMs: 10 * 60 * 1000,
    blackMs: 10 * 60 * 1000,
    thinking: false,
  };
  mode = 'play';
  overlay = null;
  // If the human plays black, white moves first — the AI (unlinked) or the
  // remote (linked, so we just wait for their move).
  if (!linked && !prefs.humanIsWhite) startThink();
}

// ---------------------------------------------------------------------------
// DOM
// ---------------------------------------------------------------------------
const el = {
  stage: document.getElementById('stage'),
  prompt: document.getElementById('prompt'),
  clock: document.getElementById('clock'),
  evalfill: document.getElementById('evalfill'),
  evaltext: document.getElementById('evaltext'),
  board: document.getElementById('board'),
  lastmove: document.getElementById('lastmove'),
  badge: document.getElementById('badge'),
  hint: document.getElementById('hint'),
  overlay: document.getElementById('overlay'),
  ovTitle: document.getElementById('ov-title'),
  ovList: document.getElementById('ov-list'),
  ovHint: document.getElementById('ov-hint'),
};

// 64 display cells, built once. Display order is always top-left to bottom-
// right; board<->display mapping handles orientation (human side at bottom).
const cells = [];
for (let i = 0; i < 64; i++) {
  const div = document.createElement('div');
  div.setAttribute('role', 'gridcell');
  el.board.appendChild(div);
  cells.push(div);
}
const flip = () => !prefs.humanIsWhite;
const dispToBoard = (i) => flip()
  ? { r: 7 - Math.floor(i / 8), c: 7 - (i % 8) }
  : { r: Math.floor(i / 8), c: i % 8 };

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
const fileChar = (c) => 'abcdefgh'[c];
const rankChar = (r) => '87654321'[r];
const same = (a, b) => a && b && a.r === b.r && a.c === b.c;

function renderBoard() {
  const g = state.game;
  const kingPos = R.inCheck(g, g.white) ? findKingPos(g.board, g.white) : null;
  for (let i = 0; i < 64; i++) {
    const { r, c } = dispToBoard(i);
    const div = cells[i];
    const p = g.board[r][c];
    div.className = 'sq ' + ((r + c) % 2 === 0 ? 'light' : 'dark');
    div.innerHTML = p === '.' ? '' :
      `<span class="pc ${R.isWhite(p) ? 'white' : 'black'}">${GLYPH[p]}</span>`;
    if (state.last) {
      if (same({ r, c }, state.last.from)) div.classList.add('lastsrc');
      if (same({ r, c }, state.last.to)) div.classList.add('lastdst');
    }
    if (state.targets.some((m) => m.to.r === r && m.to.c === c)) {
      div.classList.add('target');
      if (p !== '.' || state.targets.some((m) => m.to.r === r && m.to.c === c && m.ep)) div.classList.add('capture');
    }
    if (state.picked && same({ r, c }, state.picked)) div.classList.add('picked');
    if (kingPos && kingPos.r === r && kingPos.c === c) div.classList.add('check');
    if (mode === 'play' && same({ r, c }, state.cursor)) div.classList.add('cursor');
  }
}
function findKingPos(board, white) {
  const k = white ? 'K' : 'k';
  for (let r = 0; r < 8; r++) for (let c = 0; c < 8; c++) if (board[r][c] === k) return { r, c };
  return null;
}

function fmtClock(ms) {
  const s = Math.max(0, Math.round(ms / 1000));
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
}

function renderHud() {
  const g = state.game;
  const humanTurn = g.white === prefs.humanIsWhite;
  const inChk = R.inCheck(g, g.white);
  let prompt = state.thinking ? 'Thinking…'
             : (humanTurn ? 'Your move' : (linked ? 'Waiting…' : 'Waiting'));
  if (inChk && !state.thinking) prompt = humanTurn ? 'Check!' : prompt;
  el.prompt.textContent = prompt;
  el.prompt.className = 'prompt' + (prompt === 'Your move' ? '' : prompt === 'Check!' ? ' check' : ' waiting');

  // Hint line doubles as the link-status indicator.
  el.hint.textContent = linked
    ? `Linked · room ${prefs.room} · ${peerPresent ? 'desktop ✓' : 'waiting for desktop'}`
    : '◀▲ ▼▶ move · ⏎ pick · ⎋ menu';

  const myMs = prefs.humanIsWhite ? state.whiteMs : state.blackMs;
  el.clock.textContent = fmtClock(myMs);
  el.clock.classList.toggle('low', myMs < 30000);

  const cp = Math.max(-1000, Math.min(1000, state.evalWhite));
  el.evalfill.style.width = `${50 + (cp / 1000) * 50}%`;
  const pawns = (cp / 100).toFixed(1);
  el.evaltext.textContent = cp > 0 ? `+${pawns}` : pawns;

  el.lastmove.textContent = state.last ? state.last.san : '—';
  el.badge.textContent = state.badge ? state.badge.glyph : '';
  el.badge.className = 'badge ' + (state.badge ? state.badge.kind : '');
}

function render() { renderBoard(); renderHud(); renderOverlay(); }

// ---------------------------------------------------------------------------
// Overlay (promotion picker / menu / game-over)
// ---------------------------------------------------------------------------
function renderOverlay() {
  if (mode !== 'promo' && mode !== 'menu' && mode !== 'over') {
    el.overlay.classList.add('hidden');
    el.hint.style.visibility = 'visible';
    return;
  }
  el.hint.style.visibility = 'hidden';
  el.overlay.classList.remove('hidden');
  el.ovList.className = 'ov-list' + (mode === 'promo' ? ' promo' : '');
  el.ovTitle.className = 'ov-title' + (overlay.titleClass || '');
  el.ovTitle.textContent = overlay.title;
  el.ovList.innerHTML = '';
  overlay.items.forEach((it, i) => {
    const d = document.createElement('div');
    d.className = 'ov-item focusable' + (i === overlay.idx ? ' sel' : '');
    d.textContent = it.label;
    el.ovList.appendChild(d);
  });
  el.ovHint.textContent = overlay.hint || '';
}

// ---------------------------------------------------------------------------
// Cursor + move entry (play mode)
// ---------------------------------------------------------------------------
function moveCursor(visualDr, visualDc) {
  // "Visual" up/left map to board deltas depending on orientation.
  const dr = flip() ? -visualDr : visualDr;
  const dc = flip() ? -visualDc : visualDc;
  state.cursor.r = Math.max(0, Math.min(7, state.cursor.r + dr));
  state.cursor.c = Math.max(0, Math.min(7, state.cursor.c + dc));
  renderBoard();
}

function activate() {
  const g = state.game;
  if (state.thinking || g.white !== prefs.humanIsWhite) return;  // not your turn
  const { r, c } = state.cursor;
  if (!state.picked) {
    const moves = R.legalMoves(g).filter((m) => m.from.r === r && m.from.c === c);
    if (moves.length) { state.picked = { r, c }; state.targets = moves; renderBoard(); }
    return;
  }
  // Second activate: same square cancels; another own piece re-picks; a legal
  // target commits (opening the promotion picker if it's a promotion).
  if (same({ r, c }, state.picked)) { clearPick(); return; }
  const matches = state.targets.filter((m) => m.to.r === r && m.to.c === c);
  if (!matches.length) {
    const re = R.legalMoves(g).filter((m) => m.from.r === r && m.from.c === c);
    if (re.length) { state.picked = { r, c }; state.targets = re; renderBoard(); }
    else clearPick();
    return;
  }
  if (matches.length > 1 && matches[0].promo) { openPromo(matches); return; }
  commitMove(matches[0]);
}
function clearPick() { state.picked = null; state.targets = []; renderBoard(); }

function openPromo(matches) {
  mode = 'promo';
  const order = ['Q', 'R', 'B', 'N'];
  const items = order.map((u) => ({
    label: GLYPH[prefs.humanIsWhite ? u : u.toLowerCase()],
    move: matches.find((m) => m.promo && m.promo.toUpperCase() === u),
  }));
  overlay = { title: 'Promote', items, idx: 0, hint: '◀ ▶ choose · ⏎ confirm · ⎋ cancel' };
  render();
}

// ---------------------------------------------------------------------------
// Move execution + the AI reply
// ---------------------------------------------------------------------------
function commitMove(move) {
  const g = state.game;
  const san = R.toSan(g, move);
  state.prevEvalWhite = state.evalWhite;     // baseline for move-quality scoring
  state.game = R.applyMove(g, move);
  state.last = { from: move.from, to: move.to, san };
  state.badge = null;
  clearPick();
  mode = 'play';
  save();
  if (linked) window.ChessSync.sendMove(moveToUci(move));   // push to the desktop
  if (checkOver()) { render(); return; }
  if (!linked) startThink();                                // linked: wait for the remote
  render();
}

function startThink() {
  state.thinking = true;
  const d = DIFFS[prefs.difficulty];
  requestSearch(state.game, { timeMs: d.timeMs, maxDepth: d.maxDepth });
  renderHud();
}

function onBestMove(res) {
  if (!state) return;
  state.thinking = false;
  if (!res.move) { checkOver(); render(); return; }   // no reply => game already over
  const g = state.game;
  const san = R.toSan(g, res.move);
  state.game = R.applyMove(g, res.move);
  state.last = { from: res.move.from, to: res.move.to, san };
  state.evalWhite = res.evalCp;
  // Grade the human's previous move by the eval swing (white-relative cp
  // converted to the human's point of view).
  const sign = prefs.humanIsWhite ? 1 : -1;
  const loss = (state.prevEvalWhite - res.evalCp) * sign;
  state.badge = classify(loss);
  save();
  checkOver();
  render();
}

function classify(loss) {
  if (loss >= 250) return CLS.blunder;
  if (loss >= 110) return CLS.mistake;
  if (loss >= 50) return CLS.inaccuracy;
  if (loss <= -130) return CLS.good;      // found a strong move / won material
  return null;                            // unremarkable — keep the HUD clean
}

function checkOver() {
  const res = R.result(state.game);
  if (!res) return false;
  endGame(res);
  return true;
}

function endGame(res) {
  let title = 'Draw', cls = ' draw';
  if (res === 'checkmate') {
    // The side to move is checkmated, so the OTHER side won.
    const humanWon = (state.game.white !== prefs.humanIsWhite);
    title = humanWon ? 'You win!' : 'You lose';
    cls = humanWon ? ' win' : ' lose';
  } else if (res === 'stalemate') { title = 'Stalemate'; }
  else if (res === 'fifty') { title = 'Draw · 50-move'; }
  else if (res === 'insufficient') { title = 'Draw · material'; }
  mode = 'over';
  overlay = { title, titleClass: cls, items: [{ label: 'New game' }], idx: 0, hint: '⏎ new game · ⎋ menu' };
  state.thinking = false;
}

function endByFlag(whoFlaggedWhite) {
  const humanFlagged = (whoFlaggedWhite === prefs.humanIsWhite);
  mode = 'over';
  overlay = {
    title: humanFlagged ? 'You lose · time' : 'You win · time',
    titleClass: humanFlagged ? ' lose' : ' win',
    items: [{ label: 'New game' }], idx: 0, hint: '⏎ new game · ⎋ menu',
  };
  state.thinking = false;
  render();
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
function openMenu() {
  mode = 'menu';
  overlay = { title: '3D Chess', items: buildMenu(), idx: 0, hint: '▲ ▼ navigate · ⏎ select · ⎋ back' };
  render();
}
function buildMenu() {
  const linkLabel = linked ? 'Unlink desktop'
    : sync.status === 'connecting' ? 'Linking…' : 'Link to desktop';
  return [
    { label: 'New game', act: () => { newGame(); render(); } },
    { label: `Play: ${prefs.humanIsWhite ? 'White' : 'Black'}`, act: () => { prefs.humanIsWhite = !prefs.humanIsWhite; savePrefs(); newGame(); render(); } },
    { label: `Level: ${DIFFS[prefs.difficulty].name}`, hidden: linked, act: () => { prefs.difficulty = (prefs.difficulty + 1) % DIFFS.length; savePrefs(); openMenu(); } },
    { label: linkLabel, act: () => { if (linked || sync.status !== 'off') disconnectSync(); else connectSync(); openMenu(); } },
    { label: `Room: ${prefs.room}`, act: () => { prefs.room = (prefs.room % 9) + 1; savePrefs(); openMenu(); } },
    { label: 'Resume', act: () => { mode = 'play'; overlay = null; render(); } },
  ].filter((it) => !it.hidden);
}

// ---------------------------------------------------------------------------
// Input — D-pad (Neural Band swipes/pinches => arrows + Enter + Escape)
// ---------------------------------------------------------------------------
document.addEventListener('keydown', (e) => {
  const k = e.key;
  if (!['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Enter', 'Escape'].includes(k)) return;
  e.preventDefault();

  if (mode === 'promo') {
    if (k === 'ArrowLeft') overlay.idx = (overlay.idx + 3) % 4;
    else if (k === 'ArrowRight') overlay.idx = (overlay.idx + 1) % 4;
    else if (k === 'Enter') { const mv = overlay.items[overlay.idx].move; mode = 'play'; overlay = null; commitMove(mv); return; }
    else if (k === 'Escape') { mode = 'play'; overlay = null; render(); return; }
    renderOverlay();
    return;
  }
  if (mode === 'menu') {
    if (k === 'ArrowUp') overlay.idx = (overlay.idx + overlay.items.length - 1) % overlay.items.length;
    else if (k === 'ArrowDown') overlay.idx = (overlay.idx + 1) % overlay.items.length;
    else if (k === 'Enter') { overlay.items[overlay.idx].act(); return; }
    else if (k === 'Escape') { if (state) { mode = 'play'; overlay = null; render(); } }
    renderOverlay();
    return;
  }
  if (mode === 'over') {
    if (k === 'Enter') { newGame(); render(); }
    else if (k === 'Escape') { openMenu(); }
    return;
  }
  // play mode
  switch (k) {
    case 'ArrowUp': moveCursor(-1, 0); break;
    case 'ArrowDown': moveCursor(1, 0); break;
    case 'ArrowLeft': moveCursor(0, -1); break;
    case 'ArrowRight': moveCursor(0, 1); break;
    case 'Enter': activate(); break;
    case 'Escape': if (state.picked) clearPick(); else openMenu(); break;
  }
});

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------
let lastTick = performance.now();
function tick(now) {
  const dt = now - lastTick;
  lastTick = now;
  if (state && mode === 'play') {
    if (state.game.white) state.whiteMs -= dt; else state.blackMs -= dt;
    if (state.whiteMs <= 0) { state.whiteMs = 0; endByFlag(true); }
    else if (state.blackMs <= 0) { state.blackMs = 0; endByFlag(false); }
    el.clock.textContent = fmtClock(prefs.humanIsWhite ? state.whiteMs : state.blackMs);
    el.clock.classList.toggle('low', (prefs.humanIsWhite ? state.whiteMs : state.blackMs) < 30000);
  }
  requestAnimationFrame(tick);
}

// ---------------------------------------------------------------------------
// Engine: a Web Worker if available, else a synchronous fallback.
// ---------------------------------------------------------------------------
let worker = null;
function setupWorker() {
  try {
    worker = new Worker('engine-worker.js');
    worker.onmessage = (e) => { if (e.data && e.data.type === 'bestmove') onBestMove(e.data); };
    worker.onerror = () => { worker = null; };   // fall back to sync on failure
  } catch (_) { worker = null; }
}
function requestSearch(game, opts) {
  if (worker) { worker.postMessage({ type: 'search', state: game, opts }); return; }
  // Synchronous fallback (e.g. file:// where workers are blocked): yield a
  // frame so "Thinking…" paints, then search on the main thread.
  setTimeout(() => onBestMove(E.searchBestMove(game, opts)), 30);
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
function savePrefs() {
  try { localStorage.setItem('glasses_chess_prefs', JSON.stringify(prefs)); } catch (_) {}
}
function save() {
  try {
    localStorage.setItem('glasses_chess', JSON.stringify({
      game: state.game, last: state.last, badge: state.badge,
      evalWhite: state.evalWhite, prevEvalWhite: state.prevEvalWhite,
      whiteMs: state.whiteMs, blackMs: state.blackMs,
    }));
  } catch (_) {}
}
function restore() {
  try { const p = JSON.parse(localStorage.getItem('glasses_chess_prefs')); if (p) prefs = Object.assign(prefs, p); } catch (_) {}
  try {
    const s = JSON.parse(localStorage.getItem('glasses_chess'));
    if (!s || !s.game) return false;
    newGame();
    Object.assign(state, {
      game: s.game, last: s.last, badge: s.badge,
      evalWhite: s.evalWhite || 0, prevEvalWhite: s.prevEvalWhite || 0,
      whiteMs: s.whiteMs, blackMs: s.blackMs, thinking: false,
    });
    mode = 'play'; overlay = null;
    state.cursor = prefs.humanIsWhite ? { r: 6, c: 4 } : { r: 1, c: 4 };
    // If it's the AI's turn in the restored position, let it think.
    if (!R.result(state.game) && state.game.white !== prefs.humanIsWhite) startThink();
    return true;
  } catch (_) { return false; }
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------
setupWorker();
if (!restore()) newGame();
render();
requestAnimationFrame((t) => { lastTick = t; tick(t); });
