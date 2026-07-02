# 3D Chess

A polished 3D chess game you can play against the Stockfish engine — on your
desktop or right in your web browser. Pick your side, choose a difficulty from
total beginner to grandmaster, and play on a beautifully rendered board with
realistic lighting, wood textures, and reflections.

**▶ Play now in your browser (nothing to install):** <https://jaher.github.io/3d-chess/>

![Main Menu](screenshots/Menu.png)

![Game Screenshot](screenshots/Game.png)

## What you can do

- **Play against a world-class engine** at any strength — from a gentle
  beginner level up to grandmaster, set with a simple slider.
- **Choose your side and your pace** — play White or Black, with Classical,
  Rapid, Blitz, or Bullet clocks, or no clock at all.
- **Follow the game as it unfolds** — a live evaluation graph shows who's ahead,
  and the move list flags each move's quality with colour-coded marks
  (brilliant, mistake, blunder…) so you can see where the game turned. Tap a
  flagged move to see *why* it was a mistake — and the engine's whole better
  **line** (e.g. "Nf3 Nc6 Bb5"), not just the next move.
- **Replay and review** — step back through the whole game move by move at any
  time to see where it turned.
- **Solve puzzles** — built-in checkmate challenges, fork-and-pin tactics, and
  the daily puzzle straight from chess.com.
- **Build a streak** — open **Practice** from the menu and solve tactics back to
  back; a streak counter tracks how many you get in a row and remembers your
  personal best.
- **Track your progress** — the Practice screen shows a personal **tactics
  rating** that rises and falls as you solve and miss, and points out your
  **weakest area** (mates, forks, or pins) so you know what to drill. A
  one-click **"Drill your weakness"** button jumps straight into the matching
  set. It's all remembered between sessions.
- **Learn the openings** — the Practice screen lists opening drills (Italian
  Game, Ruy Lopez, Queen's Gambit, London System…). Play your side's moves and
  the book replies appear automatically; a wrong move rewinds so you can try
  the line again until it sticks.
- **Play a friend online** *(browser)* — click **Play online**, host a game and
  share the code (or paste a friend's), and play head-to-head over a direct
  peer-to-peer connection — with your opponent's **webcam in the corner**.
- **Play hands-free** — hold the spacebar and just say your move out loud
  ("knight d3", "castle kingside"), and ask for a hint by voice
  ("what should I play?").
- **Let it coach you out loud** — with spoken moves on, the game names each
  move's quality as you play and, when you slip, tells you *why* and what to
  play instead ("Blunder. Leaves the rook on a8 hanging. Better was knight to
  f3.") — hands-free learning while you play.
- **Enjoy the board** — soft shadows and realistic lighting, walnut wood, glossy
  reflective squares, a working analog clock, and a playful main menu where you
  can grab and fling the pieces around.
- **Walk through a photo on your glasses** *(experimental)* — snap a photo on
  your phone, let it grow into a 3D Gaussian-splat world, and stroll around it
  on a Meta Ray-Ban Display with the Neural Band — the server renders and
  streams every frame, the glasses just watch. Lives in its own repo:
  [jaher/splat-glasses](https://github.com/jaher/splat-glasses).

## Play in your browser

The easiest way to play — no installation, no setup. Just open:

**<https://jaher.github.io/3d-chess/>**

It runs in any modern desktop web browser.

## Install on your computer

Prefer a native app? You build it from source — it's three steps.

**1. Install the prerequisites for your system:**

- **Ubuntu / Debian**
  ```bash
  sudo apt-get install -y build-essential libgtk-3-dev libepoxy-dev libcurl4-openssl-dev pkg-config cmake
  ```
- **Fedora**
  ```bash
  sudo dnf install -y gcc-c++ make gtk3-devel libepoxy-devel libcurl-devel pkg-config cmake
  ```
- **Arch Linux**
  ```bash
  sudo pacman -S base-devel gtk3 libepoxy curl pkgconf cmake
  ```
- **macOS** (needs [Homebrew](https://brew.sh))
  ```bash
  brew install sdl2 libepoxy pango cairo glib fontconfig curl pkg-config cmake
  ```

**2. Download the game.** The `--recurse-submodules` part matters — it pulls in
the Stockfish engine the game plays against:

```bash
git clone --recurse-submodules https://github.com/jaher/3d-chess
cd 3d-chess
```

**3. Build and run it:**

```bash
make            # on macOS, use:  make -f Makefile.sdl
./chess         # on macOS, run:  ./chess_sdl
```

The very first build takes a couple of minutes — it compiles the Stockfish
engine and downloads the voice-input model. After that, starting the game is
instant.

> **Are you a developer?** Other platforms (iOS, Android, the browser build),
> the complete feature list, build options, and the project layout all live in
> **[README_TECHNICAL.md](README_TECHNICAL.md)**.

## Controls

| Control | Action |
|---------|--------|
| **Left click** | Select a piece, move to a highlighted square, or click a button |
| **Left drag** | Rotate the camera around the board |
| **Scroll wheel** | Zoom in and out |
| **Click the withdraw flag** (bottom-right) | Resign the current game |
| **Hold SPACE** (your turn) | Speak your move, then release to play it |
| **A** or **←/→** | Enter analysis mode to replay the game |
| **←/→** (in analysis) | Step back / forward one move |
| **C** (in analysis) | Toggle the square-control heatmap (who controls each square) |
| **R** | Review your mistakes — jump through them with ←/→, see the better move |
| **Escape** | Exit analysis, or close a dialog |

## How to Play

1. From the main menu, click **Start Game** to open the setup screen.
2. Pick your side, choose a **time control**, and drag the **strength** slider to
   the level you want.
3. Click **Start** — the board appears and the side you picked moves first.
4. Click one of your pieces to select it. Its legal moves appear as **blue
   rings**, and captures as **red rings** — click a ring to make the move.
5. The engine replies with an animated move. The graph and move list in the
   top-right keep track of how the game is going.
6. The game ends on checkmate, stalemate, or when a clock runs out. Click
   **Back to Menu** to play again.
7. Press **A** at any time to **replay** the game move by move with the arrow
   keys, then **Continue Playing** to pick up where you left off.
8. To resign, click the **withdraw flag** in the bottom-right corner and confirm.

## Credits & license

3D Chess is open source under the **MIT** license. It bundles the
[Stockfish](https://stockfishchess.org/) chess engine and a few third-party
assets (board model, textures, image loader), each under its own license — full
credits are in
[README_TECHNICAL.md](README_TECHNICAL.md#credits--third-party-licenses).
