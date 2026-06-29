# Meta Ray-Ban Display — Chess Web App

> **Complete, playable chess game for the Meta Ray-Ban Display.** Not yet run on
> physical hardware (the author has none), but fully functional in any browser,
> which is exactly Meta's documented "build and preview in your browser, then
> deploy via URL" loop. The platform research, rationale, and staged plan live in
> [`../docs/meta-rayban-display.md`](../docs/meta-rayban-display.md).

A chess game for the **Meta Ray-Ban Display** in-lens screen, built as a **Web
App** — the only publicly available path that renders a fully custom UI to the
Display lens, with no companion app required. It targets Meta's documented
constraints:

- **600×600 px**, no scrolling, additive lens (pure black = transparent → dark
  background, light high-contrast UI, bright accents), body text ≥16 px.
- Input is **D-pad only**: Neural Band swipes/pinches and frame cap-touch arrive
  as standard `ArrowUp/Down/Left/Right` + `Enter` + `Escape` `keydown` events;
  the menu uses the `.focusable` + cyan focus-ring convention.
- `<meta name="mrbd-web-app-capable" content="yes">` + a web-app `manifest.json`
  with PNG icons mark the page as a deployable Web App.

This is a **full integration** — no stubs:

- **Real rules** — `chess.js` is a complete legal move generator (castling, en
  passant, promotion, check/checkmate/stalemate, 50-move + insufficient-material
  draws), pinned by **perft** (depth 5 = 4 865 609, verified).
- **Real AI** — `engine.js` is an alpha-beta search with iterative deepening, a
  wall-clock budget, MVV-LVA ordering, and quiescence over a material +
  piece-square-table evaluation. It runs in a **Web Worker** so the HUD and clock
  never stall while it thinks (with a synchronous fallback for `file://`).
  Stockfish.js is deliberately *not* used — the glasses run a lightweight
  on-device browser, so a small dependency-free engine is the right fit.
- **Full game** — legal-move highlighting, a promotion picker, board flips for
  playing Black, a move-quality coach badge (`!`/`?!`/`?`/`??`) from the engine's
  eval swing, a clock with flagging, a settings menu, and `localStorage` resume.

## Files

| File | Role |
|---|---|
| `index.html` | 600×600 stage: prompt + clock, eval bar, 8×8 board, last-move + quality badge, overlay (menu/promotion/game-over) |
| `styles.css` | Additive-lens theme (black bg, cyan focus ring, white vs amber pieces so both sides read as light) |
| `chess.js` | Perft-verified rules engine (`ChessRules`); pure logic, no DOM |
| `engine.js` | Alpha-beta AI (`ChessEngine`) over material + PST eval; pure logic |
| `engine-worker.js` | Web Worker that `importScripts` chess.js + engine.js and runs the search off-thread |
| `app.js` | HUD rendering, D-pad input, game flow, move-quality coach, clock, persistence |
| `manifest.json`, `icon-*.png` | Web-app manifest + PNG icons for deployment |

## Run it locally (Meta's documented test loop)

No build step — static files. Serve the folder over HTTP and open it; the
keyboard arrow keys stand in for the D-pad:

```bash
cd glasses
python3 -m http.server 8080
# open http://localhost:8080/  (size the window to 600x600)
```

- **Arrow keys** — move the cursor (cyan ring). Pick a piece and its legal
  destinations light up as green dots/rings.
- **Enter** — pick the from-square, then Enter on a highlighted target to move
  (a promotion opens a Q/R/B/N picker).
- **Escape** — cancel a pick, or open the menu (new game · play White/Black ·
  level Easy/Normal/Hard · resume).

Self-tests: `node chess.js perft` (move-gen correctness) and
`node engine.js smoke` (the AI plays a few self-moves).

## Link to the desktop (move sync)

The glasses can mirror a game with the C++ desktop build so moves made on the
glasses appear on the computer and vice versa. The glasses **cannot use
Bluetooth** (the Web App sandbox has no Web Bluetooth API — only Neural Band,
IMU, GPS, storage), so the link is a **WebSocket** to a small relay; the desktop
joins the same room over raw **TCP**. The relay bridges both transports per room.

```bash
# 1. start the relay (pure Node, no npm deps; speaks WebSocket + TCP)
node glasses/sync-server.js            # ws :8090, tcp :8091

# 2. link the desktop to room 1
CHESS_SYNC_ROOM=1 ./chess              # CHESS_SYNC_HOST/PORT override the relay

# 3. on the glasses: menu (Escape) -> "Room: N" to match -> "Link to desktop"
```

The first peer into a room is the colour authority (the relay assigns the role),
so the link is order-independent. Files: `sync.js` (glasses client), `sync-server.js`
(relay), and `net_sync.cpp`/`.h` on the desktop side. For real glasses use the
relay must sit behind **WSS** (the glasses require HTTPS/WSS); `ws://localhost`
is fine for local testing.

## Deploy to the glasses

Meta loads Web Apps from a **public HTTPS URL** via the Meta AI app (Developer
Mode → add Web App; can be shared to up to 100 testers via a password-protected
URL). Requires recent Ray-Ban Display firmware + Meta AI app. See
[`../docs/meta-rayban-display.md`](../docs/meta-rayban-display.md) for the full
deployment notes and current developer-preview availability.
