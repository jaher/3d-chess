# Meta Ray-Ban Display glasses — HUD front-end (research + plan + scaffold)

> **Status: a complete, browser-testable Web App. NOT yet run on hardware.**
> There is a full, playable chess game under [`glasses/`](../glasses/) that
> renders a glanceable HUD into the 600×600 in-lens viewport model Meta
> specifies, driven entirely by the D-pad/Neural-Band key events the glasses
> emit. The chess rules and AI are **fully implemented** (no longer stubbed):
> `chess.js` is a perft-verified legal move generator and `engine.js` is an
> alpha-beta AI run off-thread in a Web Worker — see "What's implemented" below.
> It has **not** been run on real Ray-Ban Display hardware (the author has
> none). The Linux GTK (`make`), web (`make -C web`), native SDL2
> (`make -f Makefile.sdl`), iOS, and Android builds are **unaffected**: the app
> is static HTML/CSS/JS that no Makefile compiles, and it touches no shared
> C++ source.

This document captures (1) what the Meta developer platform actually offers for
the Ray-Ban Display glasses today, with citations; (2) which integration shape
is the right fit for this C++17/OpenGL-ES engine and why; (3) a staged plan; and
(4) the scaffold that lands with it.

---

## 1. Research findings (June 2026)

### 1.1 The platform: Meta Wearables Device Access Toolkit

Meta opened its glasses to third-party developers for the first time via the
**Meta Wearables Device Access Toolkit** ("Wearables DAT"), announced in a
developer preview. There are **two distinct developer paths**, and they are not
interchangeable:

| Path | Runs where | Language / framework | Renders to the lens? |
|---|---|---|---|
| **Web Apps** | **On-glasses** (lightweight on-device browser) | HTML / CSS / JavaScript, no proprietary framework | **Yes — full custom 600×600 UI** |
| **Native mobile SDK** (Device Access Toolkit) | **On-phone** (companion iOS/Android app) | Swift (iOS 15.2+) / Kotlin (Android 10+) | Camera/audio access; arbitrary display UI is **partner-gated / not in the public Android surface yet** |

Key facts established from the official docs:

- **Web Apps** are standard HTML/CSS/JS, **hosted on a publicly accessible HTTPS
  URL**, that the Meta AI app loads onto the glasses' on-device browser — **no
  companion app required**. This is the only publicly documented path that lets
  a third party render a fully custom UI to the Display lens.
  (<https://wearables.developer.meta.com/docs/develop/webapps/>)
- **Display model:** a **fixed 600×600 px viewport, no scrolling**. The display
  is an **additive waveguide** — pure **black renders as fully transparent**, so
  the design rules are "dark backgrounds, light high-contrast UI, bright
  accents," body text ≥16 px and primary content 20–24 px.
  (<https://wearables.developer.meta.com/docs/develop/webapps/build/>)
- **Input model:** there is **no mouse, touchscreen, or physical keyboard**.
  Neural Band sEMG gestures and frame cap-touch swipes are surfaced to the page
  as **standard `keydown` events**: `ArrowUp/Down/Left/Right` for directional
  navigation, `Enter` to select/activate, `Escape` for back. Every interactive
  element must be reachable by arrow-key cycling (Meta's samples use a
  `.focusable` class + focus-ring styling). Custom Neural Band gestures are
  **not** exposed yet — only swipes and pinches.
  (<https://wearables.developer.meta.com/docs/develop/webapps/build/>,
  <https://github.com/facebookincubator/meta-wearables-webapp>)
- **JS sensor/storage APIs available to Web Apps:** `DeviceMotionEvent` /
  `DeviceOrientationEvent` (IMU; requires `requestPermission()` from a user
  gesture), `navigator.geolocation` (GPS from the paired phone), `localStorage`
  / `sessionStorage` (5 MB each).
  (<https://wearables.developer.meta.com/docs/develop/webapps/build/>)
- **Not available to Web Apps yet:** camera, microphone, **text input**, offline
  support, notifications. SVG app icons are unsupported (use PNG ≥52×52 or
  Unicode). A `<meta name="mrbd-web-app-capable" content="yes">` tag marks the
  page as a Web App.
  (<https://wearables.developer.meta.com/docs/develop/webapps/build/>)
- **Official AI-assisted Web-App toolkit:** Meta publishes
  `facebookincubator/meta-wearables-webapp` (BSD-licensed) — a set of skills for
  Claude Code / Codex / Cursor / Copilot that scaffold `index.html` +
  `styles.css` + `app.js`, plus an `examples/snake/` reference. The Snake demo
  confirms the input model: a plain `document.addEventListener('keydown', …)`
  switching on `e.key === 'ArrowUp'` etc., a `requestAnimationFrame`/`setInterval`
  game loop, canvas-2D rendering, `localStorage` for the high score, and **no
  Meta-specific JS calls** — the device translates Neural Band EMG → arrow keys
  transparently at the browser level.
  (<https://github.com/facebookincubator/meta-wearables-webapp>)

### 1.2 The native mobile SDK (Device Access Toolkit)

- Distributed as Gradle artifacts on Android (`mwdat-core`, `mwdat-camera`,
  `mwdat-mockdevice`) and a Swift SDK on iOS. Connection flow:
  `Wearables.initialize(context)` → `Wearables.startRegistration(activity)` →
  observe `Wearables.registrationState` / `Wearables.devices`; permissions via
  `Wearables.checkPermissionStatus(Permission.CAMERA)` +
  `Wearables.RequestPermissionContract()`; sessions via
  `Wearables.createSession(AutoDeviceSelector())`.
  (<https://wearables.developer.meta.com/docs/build-integration-android/>,
  <https://github.com/facebook/meta-wearables-dat-android>,
  <https://github.com/facebook/meta-wearables-dat-ios>)
- The **publicly documented Android surface is camera streaming + photo capture
  + audio** (`StreamConfiguration`, `stream.videoStream.collect()`,
  `stream.capturePhoto()`). Meta's marketing mentions "display UI components
  including text, images, lists, buttons, and video," but rendering arbitrary UI
  to the Display lens from the native SDK is **not** in the public Android
  getting-started docs — it reads as **partner-gated**. For a fully custom chess
  HUD, the Web-App path is the dependable one.
- **Mock Device Kit** lets you develop the native SDK without hardware — but
  **"Mock Device Kit currently doesn't support display glasses."** So the native
  path can't even *simulate* the Display lens locally today.
  (<https://wearables.developer.meta.com/docs/develop/dat/mock-device-kit/>)

### 1.3 Access, availability, and constraints

- **Developer preview**, not GA. SDK downloads are global, but **full
  capabilities (including the Wearables Developers Center) are limited to AI-
  glasses-supported countries**.
  (<https://developers.meta.com/wearables/faq/>)
- **Publishing to the public is not available during the preview.** You can
  share Web Apps **by URL** and native apps via release channels to a small
  tester pool; broad publishing is partner-gated, with general availability
  slated for 2026.
  (<https://developers.meta.com/wearables/faq/>,
  <https://developers.meta.com/blog/build-for-display-glasses/>)
- **On-device prerequisites:** Meta Ray-Ban Display firmware v125+, Meta AI app
  v272+, Developer Mode enabled (tap the app version five times in Settings),
  glasses paired and connected.
  (<https://wearables.developer.meta.com/docs/develop/webapps/>)
- **Supported devices:** Ray-Ban Meta (Gen 1/2), Ray-Ban Meta (Display), Oakley
  Meta HSTN, Oakley Meta Vanguard. Only the **Display** model has a lens screen;
  the others are camera/audio-only.
  (<https://developers.meta.com/wearables/faq/>)

#### Sources

- Introducing the toolkit — <https://developers.meta.com/blog/introducing-meta-wearables-device-access-toolkit/>
- Build for Display glasses — <https://developers.meta.com/blog/build-for-display-glasses/>
- Web Apps overview — <https://wearables.developer.meta.com/docs/develop/webapps/>
- Web Apps build guide — <https://wearables.developer.meta.com/docs/develop/webapps/build/>
- DAT getting started — <https://wearables.developer.meta.com/docs/develop/dat/getting-started-toolkit/>
- Android integration — <https://wearables.developer.meta.com/docs/build-integration-android/>
- Mock Device Kit — <https://wearables.developer.meta.com/docs/develop/dat/mock-device-kit/>
- Developer FAQ — <https://developers.meta.com/wearables/faq/>
- Web-App AI toolkit (repo) — <https://github.com/facebookincubator/meta-wearables-webapp>
- Android / iOS SDK (repos) — <https://github.com/facebook/meta-wearables-dat-android>, <https://github.com/facebook/meta-wearables-dat-ios>

---

## 2. Feasibility & chosen approach

The three candidate shapes:

- **(a) Companion Android app that projects a HUD.** Reuses the existing Android
  NDK front-end and the C++ engine, talks to glasses via the native Device
  Access Toolkit. **Rejected as primary:** the public native SDK surface is
  camera/audio, not arbitrary Display UI; the Display-rendering APIs read as
  partner-gated; and **Mock Device Kit can't simulate the Display lens**, so it's
  un-testable without hardware *and* a partner slot. The existing Android
  scaffold is itself unfinished (placeholder AI), so this stacks two unknowns.
- **(b) Unity / Spatial-SDK port.** The Spatial SDK targets the Quest mixed-
  reality stack, **not** the Ray-Ban Display HUD. Re-authoring the engine in
  Unity throws away the C++17/OpenGL-ES codebase for a device whose UI is a
  600×600 monocular glanceable overlay. **Rejected** — wrong tool, wrong device.
- **(c) Glasses-as-thin-display: a Web App HUD backed by the engine.**
  **Chosen.** It is the *only* path that publicly lets a third party render a
  fully custom UI to the Display lens, it needs **no companion app**, and a
  glanceable 600×600 HUD (current position, eval bar, last move, clock, "your
  move" prompt) + D-pad/Neural-Band move entry is exactly what the form factor
  rewards. It also reuses everything this project already knows about the **web
  build**: the move-quality vocabulary, FEN/UCI, `pv_to_san`, and the vendored
  **Stockfish.js** Web Worker (`web/stockfish/`, `web/stockfish-bridge.js`).

### Why a glanceable HUD, not a 3D board in-lens

The Display is a small, monocular, additive overlay at 600×600. Re-rendering the
full PBR 3D board there would be unreadable and pointless. The HUD instead shows
what a player glances at between moves:

- an **8×8 board** drawn as high-contrast 2D with Unicode glyphs (the additive
  display makes the dark squares vanish, which actually reads well);
- an **eval bar** (who's ahead) from Stockfish centipawns;
- **last move** in SAN + a **move-quality badge** (reusing this project's
  `!!`/`!`/`?!`/`?`/`??` vocabulary);
- the **clock** and a **"your move" / "thinking…" prompt**.

Move entry uses the D-pad: a cursor walks the board (arrow keys), `Enter` picks
the from-square then the to-square, `Escape` cancels — the same two-tap idiom as
the desktop click path, mapped onto Neural Band swipes/pinches.

### Architecture (mirrors the existing front-ends)

| Concern | Desktop (`main.cpp`) | Web (`web/main_web.cpp`) | **Glasses (`glasses/app.js`)** |
|---|---|---|---|
| Render | GL 3.3 PBR 3D | WebGL2 PBR 3D | **2D HUD, 600×600 additive** (no GL) |
| Text | Cairo/Pango | stb_truetype | **DOM / CSS + Unicode glyphs** |
| Input | mouse + keys + SPACE voice | mouse + SpeechRecognition | **D-pad keydown** (`Arrow*`/`Enter`/`Escape`) from Neural Band |
| AI | Stockfish subprocess | Stockfish.js Web Worker | **JS alpha-beta engine** (`engine.js`) in a Web Worker — dependency-free, sized for the on-device browser |
| Rules | shared C++ `chess_rules` | shared C++ (WASM) | **JS rules port** (`chess.js`), perft-verified to depth 5 |
| Engine wiring shape | `AppPlatform` hooks | `AppPlatform` hooks | **same hook *contract*, JS side** (`trigger_ai_move` → worker post, reply → `app_ai_move_ready`) |

The Web App is a **new, separate front-end** — it does **not** link the C++
`AppPlatform` struct directly (that struct is for the C++ drivers). Instead it
mirrors the same *contract* in JavaScript: a "trigger AI move" that posts a FEN
to the Stockfish.js worker and a callback that applies the reply — exactly the
shape `web/stockfish-bridge.js` already implements for the main web build.

---

## 3. Staged implementation plan

**M0 — Scaffold (done).** A browser-testable `glasses/` Web App: 600×600 dark
HUD, board + eval bar + last-move + clock + "your move" prompt, D-pad cursor
move entry, `localStorage` for the in-progress game.

**M1 — Real rules + real engine (done).** The stub move application and canned
engine were replaced with a full implementation:
  - **Rules** — `glasses/chess.js` (`ChessRules`): complete legal move
    generation (castling, en passant, promotion, check/checkmate/stalemate,
    50-move + insufficient-material draws), SAN + FEN. Correctness is pinned by
    **perft** (depth 5 = 4 865 609, verified via `node chess.js perft`). A
    hand-written **JS port** was chosen over compiling the shared C++ `chess_rules`
    to WASM: the glasses run a lightweight on-device browser, so a small
    dependency-free bundle is the right fit, and perft makes the port provably
    equivalent for legality.
  - **Engine** — `glasses/engine.js` (`ChessEngine`): alpha-beta with iterative
    deepening, a wall-clock budget, MVV-LVA move ordering, and a quiescence
    search over a material + piece-square-table evaluation. It runs in a **Web
    Worker** (`engine-worker.js`, importScripts of the same two files) so the HUD
    and clock never stall while it thinks, with a synchronous fallback for
    `file://`. Stockfish.js (used by the desktop/full-web build) is deliberately
    *not* used here — too heavy for the on-device browser.
  - **Coaching** — the move-quality badge (`!`/`?!`/`?`/`??`) is derived from the
    engine's eval swing across the human's move, mirroring the desktop
    classifier's spirit.

**M2 — HUD polish for the additive lens (largely done).** Contrast/levels per
Meta's design guidance (dark bg, 20–24 px primary text, cyan focus rings on the
active square and on the `.focusable` menu rows), eval-bar animation, last-move
highlight, legal-move dots/rings, a promotion picker, board flip for playing
Black, check indication, a settings menu, and game-over states are all in. PNG
app icons + a web-app `manifest.json` and the `mrbd-web-app-capable` meta tag are
present. Remaining nice-to-have: a compact captured-material readout.

**M3 — Deploy + on-glasses test.** Host on a public HTTPS URL (Meta's docs note
Vercel + QR as the common flow), enable Developer Mode in the Meta AI app, load
the URL on the glasses, and validate the Neural Band → arrow-key mapping and
readability on the real lens. Capture findings back into this doc.

**M4 — Optional companion bridge.** *Only if* a Display partner slot opens:
add an Android Device Access Toolkit companion that pushes the live FEN/eval from
a phone game into the glasses (camera/audio scopes today; Display UI when the
partner API opens). This reuses the existing `android/` scaffold and the C++
engine; the HUD itself stays the Web App.

### What the C++ `AppPlatform` layer would need (if/when M4 lands)

The Web App path needs **nothing** from the C++ `AppPlatform` — it's a separate
JS front-end. The *companion* path (M4) would add one hook to `AppPlatform`,
following the exact pattern of the web-only `trigger_send_move`:

```c
// nullptr on every driver except the glasses-companion one (null-checked
// at call sites, like trigger_send_move). Pushes the current position +
// eval to the paired glasses HUD.
void (*push_to_glasses)(const char* fen, int eval_cp, const char* last_san);
```

Moves flow back in via a new `app_glasses_move_ready(const char* uci)` entry
point (sibling of `app_remote_move_ready` / `app_ai_move_ready`) that animates
the Neural-Band-entered move through the existing move path with no engine call —
identical plumbing to the WebRTC opponent move.

### What's mockable without hardware

- **The entire HUD + input model**: a desktop browser with the keyboard's arrow
  keys standing in for Neural Band swipes is Meta's documented local-test loop.
- **The AI/eval**: Stockfish.js runs in any browser today (it's already vendored
  for the web build).
- **Not mockable locally:** the real additive-lens readability and the actual
  Neural Band gesture feel — those need M3 hardware. (Meta's Mock Device Kit
  does **not** simulate Display glasses, so even the native path can't fake the
  lens.)

## What's implemented vs. remaining

**Implemented (M0–M2):**
- **Full legal chess rules** — `chess.js`, perft-verified to depth 5.
- **A real AI opponent** — `engine.js` alpha-beta in a Web Worker; Easy/Normal/
  Hard time budgets.
- **Complete game flow** — legal-move highlighting, promotion picker, board flip
  for Black, check/checkmate/stalemate/draw + timeout, move-quality coaching,
  clock, settings menu, `localStorage` resume.
- **Deployment assets** — `manifest.json` + PNG icons + the required meta tags.

**Remaining:**
- **Not run on hardware** — validated only in a desktop/headless browser at
  600×600. Real additive-lens readability and the Neural Band gesture feel need
  M3 hardware.
- **Nice-to-haves** — captured-material readout; opening-book variety; tuning the
  move-quality thresholds against the desktop classifier.
