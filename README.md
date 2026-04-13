# 3D Chess

A 3D chess game built with C++, GTK+3, and OpenGL. Play as white against an AI opponent powered by the Stockfish chess engine (bundled as a git submodule). Features PBR rendering with shadows, procedural wood textures, and environment reflections.

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
  chess_types.h/cpp      -- Shared types (pieces, game state, board)
  chess_rules.h/cpp      -- Game logic (moves, check, checkmate, evaluation)
  game_state.h/cpp       -- Game lifecycle (AI, analysis mode, title)
  board_renderer.h/cpp   -- OpenGL rendering (PBR, shadows, overlays)
  main.cpp               -- GTK setup, camera, input handling
  linalg.h/cpp           -- Matrix math library
  shader.h               -- GLSL shader sources (PBR, shadows, highlights, text)
  stl_model.h            -- STL 3D model loader
  ai_player.h            -- Stockfish engine integration (UCI)
  third_party/stockfish/ -- Stockfish chess engine (git submodule)
  models/                -- STL chess piece models
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
