# 3D Chess

A 3D chess game in C++ that runs natively on Linux (GTK+3 + OpenGL) and in the browser (SDL2 + WebGL 2 via Emscripten). Play either side against Stockfish at any strength from 1320 to ~2850 Elo. Features PBR rendering with shadows, procedural wood textures, environment reflections, mate-in-N challenge puzzles, and an analysis mode for replaying moves. The desktop build bundles Stockfish as a git submodule; the web build vendors a prebuilt `stockfish.js` Web Worker.

![Chess Board](https://img.shields.io/badge/OpenGL-3.3-blue) ![GTK](https://img.shields.io/badge/GTK-3.0-green) ![C++17](https://img.shields.io/badge/C++-17-orange)

![Game Screenshot](screenshots/Game.png)

![Analysis Mode](screenshots/Game2.png)

## Features

- **3D rendered chess board** with PBR (Physically Based Rendering), shadow mapping, and procedural wood grain textures
- **AI opponent** powered by Stockfish (UCI), strength configurable from ~1320 to ~2850 Elo via an in-app slider
- **Pre-game setup screen** — choose your side (White or Black), pick Stockfish strength, and pick a time control before the game starts
- **Chess clocks**: Classical (30+30), Rapid (15+10), Blitz (5+3), Bullet (1+1), or Unlimited. Live clock shown in the top-centre during play; game ends on flag fall with a "wins on time" result. Stockfish's own move time adapts to its remaining clock
- **Full chess rules**: legal move validation, check/checkmate/stalemate detection, castling, en passant, pawn promotion
- **Interactive controls**: click to select pieces, valid moves shown as animated glowing rings
- **Animated AI moves** with blue arrow indicator and smooth piece sliding
- **Score graph** (upper-right) backed by real Stockfish centipawn evaluations, tracking advantage over time; flips orientation when you play Black so your colour is always at the bottom
- **Move list** (upper-right, below the graph) in algebraic notation with check/mate suffixes, highlighting the move currently visible in analysis mode
- **Analysis mode**: step through the game move-by-move with left/right arrows (keyboard `A` to enter). "Continue Playing" and "Back to Menu" buttons in the overlay for mouse users
- **Withdraw flag**: a small wavy white cloth flag on a brown stick in the bottom-right corner. Click it to open a confirmation dialog and surrender to the main menu. Uses a 14×9 verlet cloth simulation with normal-based half-Lambert lighting (inspired by [shadertoy MldXWX](https://www.shadertoy.com/view/MldXWX))
- **Mate-in-N challenge puzzles** loaded from `challenges/*.md`, with a glass-shatter transition between puzzles and a summary page at the end. Wrong-line attempts trigger a "Mistake!" sound + board shake + Try Again button that resets the puzzle
- **Captured pieces** displayed on the sides of the board
- **Board coordinates** (a-h, 1-8) rendered with anti-aliased fonts (Cairo/Pango on desktop, `stb_truetype` in the browser)
- **Interactive main menu** — grab and fling the tumbling chess pieces around; release velocity follows the cursor/finger trajectory
- **Options screen** — reached from the main menu **Options** button; currently toggles the cartoon-outline post-process used during gameplay (also bindable to the **S** key while playing)

## Dependencies

### Ubuntu / Debian

```bash
sudo apt-get install -y \
    build-essential \
    libgtk-3-dev \
    libepoxy-dev \
    pkg-config
```

### Fedora

```bash
sudo dnf install -y \
    gcc-c++ make \
    gtk3-devel \
    libepoxy-devel \
    pkg-config
```

### Arch Linux

```bash
sudo pacman -S \
    base-devel \
    gtk3 \
    libepoxy \
    pkgconf
```

### macOS

Install the dependencies via [Homebrew](https://brew.sh):

```bash
brew install gtk+3 libepoxy pkg-config
```

The Makefile auto-detects Darwin and prepends Homebrew's pkgconfig directory
(both Apple Silicon `/opt/homebrew` and Intel `/usr/local` are handled via
`brew --prefix`).

> **Heads-up:** GTK3 on macOS uses a Quartz backend (no XQuartz needed) and
> compiles cleanly with the bundled Stockfish, but it is treated as a
> second-class target by upstream GTK. The renderer requests an OpenGL
> compatibility profile, while macOS only ships Core profile 3.2/4.1 — so
> while the build works, the GL rendering may need tweaks before the game
> displays correctly on a Mac. Patches welcome.

## Cloning

Clone recursively so that the Stockfish submodule is fetched:

```bash
git clone --recurse-submodules https://github.com/jaher/3d-chess
```

If you already cloned without `--recurse-submodules`, run:

```bash
git submodule update --init --recursive
```

## Building

```bash
make
```

The first build compiles Stockfish from source and downloads its NNUE network file, which takes a minute or two. Subsequent builds are incremental.

## Running

```bash
./chess
```

Optionally specify a different models directory:

```bash
./chess /path/to/stl/models
```

### Running the unit tests (optional)

Pure-logic unit tests (chess rules, FEN/UCI helpers, linear algebra, FEN parser) live in `tests/` and are built with a vendored [doctest](https://github.com/doctest/doctest) single-header. Run from the repo root:

```bash
make test
```

The test binary only links against the pure-C++ layer — no GL, GTK, SDL, or Stockfish subprocess — so it builds and runs in under a second.

### Tuning the AI (optional)

- `CHESS_AI_ELO` — Stockfish `UCI_Elo` value (default `1400`). The in-app pregame slider overrides this for normal play. Lower is weaker; minimum useful value is `1320`.
- `CHESS_AI_MOVETIME_MS` — forces Stockfish's per-move thinking budget in milliseconds. When unset (default) Stockfish uses either its legacy 800 ms cap in Unlimited mode, or ~1/30 of its remaining clock (clamped to `[200, 3000]` ms) when a time control is active.
- `CHESS_EVAL_MOVETIME_MS` — milliseconds spent evaluating each position for the score graph (default `150`).
- `CHESS_STOCKFISH_PATH` — path to a custom Stockfish binary. If unset, the app first looks for `./third_party/stockfish/src/stockfish`, then falls back to the `stockfish` binary on `$PATH`.

A system-installed `stockfish` (e.g. via `apt-get install stockfish`) is used automatically as a fallback if the vendored binary isn't available.

## Browser / WebAssembly version

The same game also runs in a browser, compiled to WebAssembly via
[Emscripten](https://emscripten.org). It uses **WebGL 2** for the renderer
and a vendored single-threaded build of
[Stockfish.js](https://github.com/nmrugg/stockfish.js) running inside a
Web Worker for the AI. No `SharedArrayBuffer` / COOP-COEP setup required,
so it deploys on plain GitHub Pages.

**Live demo:** <https://jaher.github.io/3d-chess/> (auto-deployed from `main`
by `.github/workflows/deploy-pages.yml`)

### Prerequisites

- A working Emscripten toolchain (`em++` on `$PATH`). On Debian/Ubuntu:
  ```bash
  sudo apt install emscripten
  ```
  Other platforms: install via the [emsdk](https://emscripten.org/docs/getting_started/downloads.html) and `source ./emsdk_env.sh`.
- Python 3 (for the local development server).

### Building

```bash
cd web
make
```

The Makefile compiles the shared C++ rendering / rules code together with
the web-only platform layer (`web/main_web.cpp`, `web/ai_player_web.cpp`,
`web/font_atlas_stb.cpp`) and produces `chess.html`, `chess.js`, `chess.wasm`,
and `chess.data` in `web/`. The first build takes 1–2 minutes; subsequent
builds are incremental.

> **Debian-package quirk:** the system Emscripten config at
> `/usr/share/emscripten/.emscripten` sets `FROZEN_CACHE = True` and stores
> the cache under `/usr/share/emscripten/cache/` which is not user-writable,
> so the SDL2 port can't be fetched on first build. Workaround: copy the
> system cache to a writable location and use a custom config:
> ```bash
> cp -r /usr/share/emscripten/cache ~/.emscripten_cache
> cat > ~/.emscripten <<'EOF'
> EMSCRIPTEN_ROOT = '/usr/share/emscripten'
> LLVM_ROOT = '/usr/bin'
> BINARYEN_ROOT = '/usr'
> NODE_JS = '/usr/bin/node'
> JAVA = 'java'
> FROZEN_CACHE = False
> CLOSURE_COMPILER = 'closure-compiler'
> LLVM_ADD_VERSION = '15'
> CLANG_ADD_VERSION = '15'
> CACHE = '/home/<your-username>/.emscripten_cache'
> EOF
> EM_CONFIG=~/.emscripten make
> ```
> Once the SDL2 port is fetched into your writable cache, subsequent
> `EM_CONFIG=~/.emscripten make` runs are fast.

### Running locally

Browsers refuse to load WebAssembly from `file://` URLs, so serve the
`web/` directory over HTTP:

```bash
cd web
make serve            # python3 -m http.server 8000
```

Then open <http://localhost:8000/chess.html>.

### Deploying to GitHub Pages

A workflow at `.github/workflows/deploy-pages.yml` handles this
automatically: every push to `main` builds the WebAssembly target on a
GitHub-hosted runner and deploys the resulting `web/` directory to Pages.

**One-time repo setup:**
1. Open **Settings → Pages**.
2. Set **Source** to **GitHub Actions**.
3. Push to `main` (or run the workflow manually from the Actions tab) —
   the first deploy takes ~3 minutes because Emscripten has to fetch
   SDL2 source on the runner.

After that, every push to `main` redeploys automatically and the site
stays at <https://YOUR-USER.github.io/REPO/>. No special HTTP headers
required — the lite single-threaded Stockfish.js build doesn't need
`SharedArrayBuffer` or COOP-COEP.

**Manual deployment (alternative):** if you'd rather host the files
yourself, the `web/` directory is fully self-contained after `make`.
Copy `chess.{html,js,wasm,data}`, `stockfish-bridge.js`, and
`stockfish/{stockfish.js,stockfish.wasm}` to any static host and serve
`chess.html` (or rename it to `index.html`).

### Decimated + packed models (`models-web/`, `models-web-packed/`)

The desktop `models/` directory holds heavily-tessellated STL pieces
(~250 MB total — fine for a native build but unworkable for a browser
asset bundle). The web build uses a two-step pipeline:

1. `tools/decimate_models.py` (Blender, quadric collapse) decimates
   `models/` → `models-web/` at ~80,000 triangles per piece —
   enough for the knight's mane and crown finials to stay crisp
   under close zoom.
2. `tools/pack_meshes.py` (Python stdlib) collapses duplicate
   vertices into an indexed mesh, drops the unused STL face normals
   (the runtime recomputes smooth normals), and gzips the result,
   producing `models-web-packed/` at ~4 MB total. The C++ loader
   sniffs the gzip magic on open, falling back to raw STL otherwise.

The web Makefile preloads `models-web-packed/` into `chess.data`;
the desktop build still reads `models/` directly.

To regenerate after editing `models/`:

```bash
blender --background --python tools/decimate_models.py  # → models-web/
python3 tools/pack_meshes.py                            # → models-web-packed/
```

(Blender 4.x or newer; Python 3.)

### Limitations vs the desktop build

- **Single-threaded Stockfish**: ~5× slower per node than threaded
  Stockfish, but at the default ELO cap (1400) that's still strong
  enough to play interesting games. In Bullet with adaptive move
  time it will occasionally lose on time against a fast human.
- **Lower-poly pieces**: the web build uses the packed 80k-tri
  meshes instead of the desktop's `models/` (~1M triangles per
  piece). Differences are invisible at normal zoom and only become
  noticeable at extreme close-ups.
- **Requires WebGL 2**: every modern browser since 2017 supports it
  (Chrome/Edge/Firefox/Safari/Opera). No fallback to WebGL 1.
- **Audio unlocks on first user gesture**: browsers suspend the Web
  Audio context until a real click/tap/keypress. The status bar
  appends "Click to enable sound" until then; the intro music picks
  up on the first interaction.

### How it differs from the desktop build

The vast majority of the game — chess rules, rendering, app/state
machine, input handling, physics, puzzles, time controls, cloth
flag, etc. — is a shared C++ layer (`chess_rules`, `board_renderer`,
the per-screen render modules (`menu_physics`, `menu_input`,
`challenge_ui`, `pregame_ui`, `shatter_transition`, `text_atlas`),
`shader`, `stl_model`, `compression`, `vec` / `mat`, `chess_types`,
`app_state`, `game_state`, `challenge`, `cloth_flag`,
`time_control`, `ai_player`) that compiles for both targets. Only
the thin platform driver differs:

1. **Platform layer** — `web/main_web.cpp` replaces `main.cpp` (SDL2 +
   `emscripten_set_main_loop` instead of GTK signals +
   `gtk_widget_add_tick_callback`). Both drivers fill in an
   `AppPlatform` hook table (see `app_state.h`) that the shared code
   calls through for time, redraws, title bar, AI dispatch — so the
   UI logic never touches GTK, SDL, Emscripten or any threading
   primitive directly.
2. **Engine** — `web/ai_player_web.cpp` posts UCI commands to a Web
   Worker via `EM_JS` instead of forking a Stockfish subprocess. The
   FEN/UCI helper functions in `ai_player.cpp` are reused via the
   `AI_PLAYER_HELPERS_ONLY` compile flag.
3. **Font atlas** — `web/font_atlas_stb.cpp` rasterises glyphs with
   `stb_truetype` from a vendored `DejaVuSans-Bold.ttf` instead of
   Cairo/Pango (which doesn't run in Emscripten).

Shaders use `#version 300 es` (matching WebGL 2) under `__EMSCRIPTEN__`
and `#version 330 core` on desktop, switched via a tiny header macro in
`shader.cpp`.

## Controls

| Control | Action |
|---------|--------|
| **Left click** | Select a piece / move to highlighted square / click UI button |
| **Left drag** | Rotate camera around the board |
| **Scroll wheel** | Zoom in/out |
| **Click withdraw flag** | Open the "Withdraw from game?" confirmation modal (bottom-right corner) |
| **A** or **Left/Right arrow** | Enter analysis mode |
| **Left arrow** (analysis) | Step back one move |
| **Right arrow** (analysis) | Step forward one move |
| **Escape** (analysis) | Exit analysis mode and return to live game |
| **Escape** (modal / dropdown) | Close the dialog or collapse the dropdown |

## How to Play

1. From the main menu click **Start Game** to open the pre-game setup screen.
2. Pick your side (**White moves first** toggles to **Black moves first**), pick a **time control** from the dropdown (Classical, Rapid, Blitz, Bullet, or Unlimited — default is Classical 30+30), and drag the **Stockfish strength** slider to your preferred Elo (1320–2850).
3. Click **Start**. The board appears, the clock starts ticking, and the side you picked moves first.
4. Click a piece to select it — a blue pulsing ring appears around it. Valid moves are shown as **blue rings** (moves) or **red rings** (captures). Click a valid square to move your piece.
5. Stockfish responds with an animated move. The score graph in the top-right tracks the evaluation in centipawns; the move list below it records the game in algebraic notation.
6. The game ends on checkmate, stalemate, or when a clock reaches zero (if a time control is active). Click **Back to Menu** in the game-over overlay to play again.
7. Press **A** at any time to enter **analysis mode** and step through the game with the arrow keys. **Continue Playing** resumes from where you left off; **Back to Menu** ends the game.
8. If you want to resign, click the **withdraw flag** in the bottom-right corner and confirm — you'll be taken straight back to the main menu.

## Project Structure

```
3d_chess/
  # Core (platform-agnostic, compiles for both desktop and web)
  chess_types.h/cpp        -- Shared types (pieces, game state, board)
  chess_rules.h/cpp        -- Game logic (moves, check, mate, eval)
  game_state.h/cpp         -- Per-game lifecycle (reset, analysis enter/exit)
  challenge.h/cpp          -- Mate-in-N puzzle loader + FEN parser
  time_control.h/cpp       -- TimeControl enum + TIME_CONTROLS[] table
  app_state.h/cpp          -- UI state machine (modes, input dispatch,
                              tick, render orchestration) — thin per-mode
                              handlers delegate to the screens below

  # Renderer
  board_renderer.h/cpp     -- Main 3D game scene (PBR, shadows, AI arrow,
                              highlights) + HUD helpers (score graph,
                              move list, clock, flag, withdraw modal,
                              game-over overlay) + renderer_draw_menu
  render_internal.h        -- Shared GL globals + text helpers that the
                              per-screen render modules link against
  menu_physics.h/cpp       -- Menu piece tumble + sub-box OBB collision
  menu_input.h/cpp         -- Menu ray-pick + drag-to-fling gesture
  pregame_ui.h/cpp         -- Pregame screen (slider, dropdown, Start)
  challenge_ui.h/cpp       -- Challenge select / overlay / next / try-again
                              / summary table
  options_ui.h/cpp         -- Options screen (cartoon-outline toggle)
  shatter_transition.h/cpp -- Voronoi glass-break puzzle transition
  text_atlas.h/cpp         -- Font atlas (Cairo/Pango | stb_truetype) +
                              NDC glyph quad helpers
  shader.h/cpp             -- GLSL sources (GL 3.30 / GLSL ES 3.00)
  stl_model.h/cpp          -- STL / packed-IMSH loader
  compression.h/cpp        -- Gzip inflate wrapper (zlib)
  cloth_flag.h/cpp         -- Verlet cloth sim + half-Lambert shading
  vec.h/cpp                -- Vec3 / Vec4 + dot / length / normalize
  mat.h/cpp                -- Mat4 + transforms / inverse / normal matrix

  # AI
  ai_player.h/cpp          -- Stockfish UCI integration
                              (subprocess on desktop; helpers shared)

  # Desktop driver
  main.cpp                 -- GTK+3 window, GtkGLArea, event wiring

  # Assets
  third_party/stockfish/   -- Native Stockfish engine (git submodule)
  models/                  -- High-res STL piece models (desktop build)
  models-web/              -- Decimated STL pieces (~80k tris, intermediate)
  models-web-packed/       -- Gzipped indexed-mesh packed pieces (~4 MB total,
                              preloaded by the web build)
  sounds/                  -- WAVs (move / capture / check / mistake /
                              glass-break / intro music)
  challenges/              -- Puzzle definition files
  screenshots/             -- Images used in this README

  # Tools
  tools/decimate_models.py -- Blender: models/ → models-web/
  tools/pack_meshes.py     -- Python:  models-web/ → models-web-packed/
  tools/homework_wizard.py -- GTK wizard for image → FEN puzzles (Gemini)
  tools/image_to_fen.py    -- CLI for image → FEN recognition
  tools/fen_to_images.py   -- CLI for FEN → rendered diagram PNGs

  # Tests
  tests/                   -- doctest-based unit tests (chess rules,
                              FEN/UCI, challenge loader, linalg). Run
                              with `make test` from this dir.
    doctest.h                 vendored single-header test framework
    chess_rules_test.cpp
    ai_player_helpers_test.cpp
    challenge_test.cpp
    linalg_test.cpp
    helpers.h                 state_from_fen() test fixture helper
    Makefile

  # CI
  .github/workflows/       -- Deploy WebAssembly build to GitHub Pages

  # Web driver
  web/                     -- WebAssembly / WebGL 2 build (Emscripten)
    main_web.cpp           --   SDL2 + emscripten_set_main_loop driver
    ai_player_web.cpp      --   JS bridge to Stockfish.js Web Worker
    font_atlas_stb.cpp     --   stb_truetype font atlas baker
    stb_truetype.h         --   vendored single-header font rasterizer
    DejaVuSans-Bold.ttf    --   vendored TTF used by the atlas
    index.html             --   HTML shell (status div + canvas +
                                 audio-unlock listeners)
    stockfish-bridge.js    --   Lazy-loaded Worker glue for Stockfish
    stockfish/             --   vendored prebuilt nmrugg/stockfish.js v18
    Makefile               --   em++ build rules (per-TU objects for -jN)
```

## Rendering

- **Cook-Torrance BRDF** with GGX distribution, Smith geometry, Fresnel-Schlick
- **Shadow mapping** (4096x4096) with 5x5 PCF soft shadows
- **Procedural environment** with studio-style lighting for reflections
- **ACES filmic tone mapping** with gamma correction
- **Procedural wood grain** using 6-octave FBM noise with medullary rays

## Upgrading Stockfish

```bash
cd third_party/stockfish && git pull origin master && cd ../..
git add third_party/stockfish
git commit -m "Bump Stockfish"
```

## License

MIT
