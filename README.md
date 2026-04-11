# 3D Chess

A 3D chess game built with C++, GTK+3, and OpenGL. Play as white against an AI opponent powered by the Anthropic Claude API. Features PBR rendering with shadows, procedural wood textures, and environment reflections.

![Chess Board](https://img.shields.io/badge/OpenGL-3.3-blue) ![GTK](https://img.shields.io/badge/GTK-3.0-green) ![C++17](https://img.shields.io/badge/C++-17-orange)

## Features

- **3D rendered chess board** with PBR (Physically Based Rendering), shadow mapping, and procedural wood grain textures
- **AI opponent** (black pieces) powered by Claude via the Anthropic API
- **Full chess rules**: legal move validation, check/checkmate detection, castling, pawn promotion
- **Interactive controls**: click to select pieces, valid moves shown as animated glowing rings
- **Animated AI moves** with blue arrow indicator and smooth piece sliding
- **Score graph** (upper-right) tracking material advantage over time with win percentages
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
    libcurl4-openssl-dev \
    pkg-config
```

### Fedora

```bash
sudo dnf install -y \
    gcc-c++ make \
    gtk3-devel \
    libepoxy-devel \
    libcurl-devel \
    pkg-config
```

### Arch Linux

```bash
sudo pacman -S \
    base-devel \
    gtk3 \
    libepoxy \
    curl \
    pkgconf
```

## Building

```bash
make
```

## Running

Set your Anthropic API key and run:

```bash
export ANTHROPIC_API_KEY="your-api-key-here"
./chess
```

Optionally specify a different models directory:

```bash
./chess /path/to/stl/models
```

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
  mat4.h                 -- Matrix math library
  shader.h               -- GLSL shader sources (PBR, shadows, highlights, text)
  stl_model.h            -- STL 3D model loader
  ai_player.h            -- Anthropic API integration
  models/                -- STL chess piece models
```

## Rendering

- **Cook-Torrance BRDF** with GGX distribution, Smith geometry, Fresnel-Schlick
- **Shadow mapping** (4096x4096) with 5x5 PCF soft shadows
- **Procedural environment** with studio-style lighting for reflections
- **ACES filmic tone mapping** with gamma correction
- **Procedural wood grain** using 6-octave FBM noise with medullary rays

## License

MIT
