# Meta Ray-Ban Display HUD — Web App scaffold (M0)

> **Scaffold only — NOT run on hardware. Chess rules + AI engine are stubbed.**
> The full research, the rationale for the Web-App path, the staged plan, and
> the C++ `AppPlatform` touch-points live in
> [`../docs/meta-rayban-display.md`](../docs/meta-rayban-display.md).

A glanceable chess HUD for the **Meta Ray-Ban Display** in-lens screen, built as
a **Web App** (the only publicly available path that renders a fully custom UI to
the Display lens — no companion app required). It targets Meta's documented
constraints:

- **600×600 px**, no scrolling, additive lens (pure black = transparent → dark
  background, light high-contrast UI, bright accents).
- Input is **D-pad only**: Neural Band swipes/pinches and frame cap-touch arrive
  as standard `ArrowUp/Down/Left/Right` + `Enter` + `Escape` `keydown` events.
- `<meta name="mrbd-web-app-capable" content="yes">` marks the page as a Web App.

## Files

| File | Role |
|---|---|
| `index.html` | 600×600 stage: prompt + clock, eval bar, 8×8 board, last-move + quality badge, control hint |
| `styles.css` | Additive-lens theme (black bg, cyan focus ring, high-contrast glyphs) |
| `app.js` | Board model, rendering, D-pad input, clock, `localStorage`, and the two `M1 SEAM`s (rules, engine) |

## Run it locally (Meta's documented test loop)

No build step — it's static files. Serve the folder over HTTP and open it; use
the keyboard arrow keys as the D-pad:

```bash
cd glasses
python3 -m http.server 8080
# open http://localhost:8080/  (size the window to 600x600)
```

- **Arrow keys** — move the square cursor (cyan ring).
- **Enter** — pick the from-square (must hold a piece of the side to move), then
  Enter again on the destination to play the move.
- **Escape** — cancel the current pick.

The demo plays a couple of canned Black replies so the turn-flow is visible.

## What's stubbed (becomes real in M1)

- **`applyMoveStub()`** — no legality; it just relocates the piece. M1 swaps in
  the shared C++ `chess_rules` compiled to WASM (FEN in / legal check / SAN out)
  so the HUD matches the desktop and web builds exactly.
- **`requestEngineMove()`** — canned replies + placeholder eval. M1 wires the
  vendored **Stockfish.js** worker (`../web/stockfish/`,
  mirroring `../web/stockfish-bridge.js`); `onEngineMove()` stays the callback.

## Deploy to the glasses (M3, needs hardware)

Meta loads Web Apps from a **public HTTPS URL** via the Meta AI app (Developer
Mode → add Web App; Vercel + QR is the common flow). Requires Ray-Ban Display
firmware v125+ and Meta AI app v272+. See `../docs/meta-rayban-display.md` §3.
