# 3D Chess

A 3D chess game in C++ that runs natively on Linux (GTK+3 + OpenGL) and in the browser (SDL2 + WebGL 2 via Emscripten). Play either side against Stockfish at any strength from 1320 to ~2850 Elo. Features PBR rendering with shadows, procedural wood textures, environment reflections, mate-in-N challenge puzzles, and an analysis mode for replaying moves. The desktop build bundles Stockfish as a git submodule; the web build vendors a prebuilt `stockfish.js` Web Worker.

![Chess Board](https://img.shields.io/badge/OpenGL-3.3-blue) ![GTK](https://img.shields.io/badge/GTK-3.0-green) ![C++17](https://img.shields.io/badge/C++-17-orange)

![Game Screenshot](screenshots/Game.png)

![Analysis Mode](screenshots/Game2.png)

## Features

- **3D rendered chess board** with PBR (Physically Based Rendering), shadow mapping, and procedural wood grain textures
- **AI opponent** (black pieces) powered by Stockfish (UCI), throttled to beginner strength
- **Full chess rules**: legal move validation, check/checkmate detection, castling, pawn promotion
- **Interactive controls**: click to select pieces, valid moves shown as animated glowing rings
- **Animated AI moves** with blue arrow indicator and smooth piece sliding
- **Score graph** (upper-right) backed by real Stockfish centipawn evaluations, tracking advantage over time
- **Analysis mode**: step through the game move-by-move with left/right arrows
- **Captured pieces** displayed on the sides of the board
- **Board coordinates** (a-h, 1-8) rendered with anti-aliased fonts via Cairo/Pango

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

### Tuning the AI (optional)

- `CHESS_AI_ELO` — Stockfish `UCI_Elo` value (default `1400`). Lower is weaker; minimum useful value is `1320`.
- `CHESS_AI_MOVETIME_MS` — milliseconds Stockfish thinks per move (default `800`).
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

### Decimated models (`models-web/`)

The desktop `models/` directory holds heavily-tessellated STL pieces
(~250 MB total — fine for a native build but unworkable for a browser
asset bundle). The web build preloads `models-web/` instead, which
contains the same pieces decimated to ~5,000 triangles each via Blender's
quadric collapse decimator. Total payload drops from ~250 MB to ~1.5 MB
with no perceptible visual difference at gameplay zoom.

To regenerate after editing `models/`:

```bash
blender --background --python tools/decimate_models.py
```

(Blender 4.x or newer.)

### Limitations vs the desktop build

- **Single-threaded Stockfish**: ~5× slower per node than threaded
  Stockfish, but at the default 800 ms/move and `UCI_Elo 1400` cap that's
  still strong enough to play interesting games.
- **Lower-poly pieces**: the web build uses `models-web/` (~5K triangles
  per piece) instead of the desktop's `models/`. Differences are
  invisible at normal zoom but visible if you zoom way in.
- **Mouse + keyboard only**: touch input on mobile is not yet wired up.
- **Requires WebGL 2**: every modern browser since 2017 supports it
  (Chrome/Edge/Firefox/Safari/Opera). No fallback to WebGL 1.

### How it differs from the desktop build

The same shared C++ code (`chess_rules`, `board_renderer`, `shader`,
`stl_model`, `linalg`, `chess_types`, `challenge`) compiles for both
targets. Three things differ:

1. **Platform layer** — `web/main_web.cpp` replaces `main.cpp` +
   `game_state.cpp` (SDL2 + `emscripten_set_main_loop` instead of GTK
   signals + `gtk_widget_add_tick_callback`).
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
| **Left click** | Select a piece / move to highlighted square |
| **Left drag** | Rotate camera around the board |
| **Scroll wheel** | Zoom in/out |
| **A** or **Left/Right arrow** | Enter analysis mode |
| **Left arrow** (analysis) | Step back one move |
| **Right arrow** (analysis) | Step forward one move |
| **Escape** (analysis) | Exit analysis mode and return to live game |

## How to Play

1. You play as **white** (bottom of the board)
2. Click a piece to select it -- a blue pulsing ring appears around it
3. Valid moves are shown as **blue rings** (moves) or **red rings** (captures)
4. Click a valid square to move your piece
5. The **AI** (black) will think and respond with an animated move
6. The game ends when a king is captured or checkmate is reached

## Project Structure

```
3d_chess/
  chess_types.h/cpp        -- Shared types (pieces, game state, board)
  chess_rules.h/cpp        -- Game logic (moves, check, checkmate, evaluation)
  game_state.h/cpp         -- Desktop game lifecycle (GTK title, AI dispatch)
  board_renderer.h/cpp     -- OpenGL rendering (PBR, shadows, overlays)
  main.cpp                 -- GTK setup, camera, input handling (desktop)
  linalg.h/cpp             -- Matrix math library
  shader.h/cpp             -- GLSL sources (compiles to GL 3.30 / GLSL ES 3.00)
  stl_model.h/cpp          -- STL 3D model loader
  ai_player.h/cpp          -- Stockfish UCI integration (subprocess on desktop)
  challenge.h/cpp          -- Mate-in-N puzzle loader
  third_party/stockfish/   -- Native Stockfish engine (git submodule)
  models/                  -- High-res STL piece models (desktop build)
  models-web/              -- Decimated STL piece models (web build)
  tools/decimate_models.py -- Blender script that produces models-web/
  challenges/              -- Puzzle definition files
  .github/workflows/       -- CI: deploy WebAssembly build to GitHub Pages
  web/                     -- WebAssembly / WebGL 2 build (Emscripten)
    main_web.cpp           --   SDL2 + emscripten_set_main_loop driver
    ai_player_web.cpp      --   JS bridge to Stockfish.js Web Worker
    font_atlas_stb.cpp     --   stb_truetype font atlas baker
    stb_truetype.h         --   vendored single-header font rasterizer
    DejaVuSans-Bold.ttf    --   vendored TTF used by the atlas
    index.html             --   HTML shell with status div + canvas
    stockfish-bridge.js    --   Worker glue (UCI ↔ WASM via Module.ccall)
    stockfish/             --   vendored prebuilt nmrugg/stockfish.js v18
    Makefile               --   em++ build rules
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
