# Native macOS build (SDL2 driver)

3D Chess ships two desktop drivers over the same platform-agnostic engine:

| Driver        | Windowing | Build                       | Notes                              |
|---------------|-----------|-----------------------------|------------------------------------|
| `main.cpp`    | GTK+3     | `make` / `make chess`       | Default on Linux                   |
| `main_sdl.cpp`| SDL2      | `make -f Makefile.sdl`      | **Recommended on macOS**, GTK-free |

GTK on macOS is a second-class, Quartz-backed target, and the renderer wants a
modern OpenGL **core** profile that GTK's GL area doesn't reliably hand it on a
Mac. The SDL2 driver sidesteps all of that: it asks SDL for the highest core
profile the OS supports — **4.3 on Linux/Windows** (compute shaders available)
and **4.1 on macOS** (Apple's ceiling; macOS exposes only 3.2 / 3.3 / 4.1 core
and **no compute shaders**) — and runs its own
`while (running) { poll; tick; render; swap; }` loop.

Everything gameplay-related (rules, AI, renderer, voice, Chessnut bridge) is
the exact same shared code the Linux and web builds use. Only the windowing,
input mapping, main loop, and worker-thread → main-thread marshalling live in
`main_sdl.cpp`.

---

## 1. Install dependencies (Homebrew)

```bash
brew install sdl2 libepoxy pango cairo glib fontconfig curl pkg-config cmake
```

What each is for:

- **sdl2** — window + GL context + input + audio device (the `audio.cpp` mixer
  already uses SDL audio).
- **libepoxy** — runtime OpenGL function loading (no GLEW needed; epoxy resolves
  against the current context automatically).
- **pango / cairo / glib / fontconfig** — these are **not** the GTK widget
  toolkit. They are the shared renderer's text rasterizer (`text_atlas.cpp`
  bakes its glyph atlas with Pango/Cairo) and the monotonic timer GLib provides
  in `board_renderer.cpp`. The web build swaps in `stb_truetype`; the desktop
  build (GTK *and* SDL) uses Pango/Cairo, so they're required here too.
- **curl** — chess.com puzzle fetch.
- **cmake** — builds the bundled whisper.cpp (voice) and SimpleBLE (Chessnut)
  static libs.

The Stockfish, whisper.cpp, and SimpleBLE submodules build from source on the
first `make -f Makefile.sdl` (same machinery as the root Makefile). Clone with
`--recurse-submodules` (or run `git submodule update --init --recursive`).

## 2. Build and run

```bash
make -f Makefile.sdl -j8        # produces ./chess_sdl
./chess_sdl                     # optional: ./chess_sdl /path/to/stl/models
```

`Makefile.sdl` auto-detects the OS with `uname -s`:

- **Darwin** → links `-framework OpenGL -framework Cocoa`, gets SDL2 from
  `sdl2-config --cflags --libs`, and pulls epoxy / pango / cairo / glib /
  fontconfig / curl from Homebrew's `pkg-config` (the Homebrew prefix is
  auto-prepended to `PKG_CONFIG_PATH`, handling both Apple-Silicon
  `/opt/homebrew` and Intel `/usr/local`). SimpleBLE links against
  `-framework CoreBluetooth -framework Foundation`, and ggml's CPU backend
  against `-framework Accelerate`.
- **Linux** → links `-lGL -lSDL2 -lepoxy` + dbus-1 (for SimpleBLE/BlueZ),
  identical library set to the GTK build minus GTK itself.

Objects are compiled into `obj-sdl/`, completely separate from the GTK build's
root-level `.o` files, so `make chess`, `make -C web`, and
`make -f Makefile.sdl` never interfere with one another.

## 3. The OpenGL 4.1 / no-compute / per-quad-splat limitation

macOS's OpenGL tops out at **4.1 core** and has **no compute shaders**. The
Gaussian-splat environment (the photoreal "Medieval Room" / "Sagrada Família"
backdrops) can be rasterized two ways:

1. a **GL-compute tile rasterizer** (`gl_raster/`), which needs **GL 4.3** —
   unavailable on macOS; and
2. a **per-quad, WebGL2-style path** (the same one the browser build uses),
   which only needs GL 3.3.

`main_sdl.cpp` forces path (2) **on macOS only**, before the renderer reads the
flag:

```c
#ifdef __APPLE__
    setenv("CHESS_GL_COMPUTE_SPLATS", "0", /*overwrite=*/0);
#endif
```

On Linux/Windows the flag is left at its default (ON), so the 4.3 context runs
the GL-compute tile rasterizer — the SDL2 build is then **pixel-identical to
the GTK build**. On macOS the 4.1 context can't run compute, so the per-quad
backdrop is used.

Other macOS-driven choices in `main_sdl.cpp`:

- The context is requested as **4.3 core on Linux/Windows, 4.1 core on macOS**,
  forward-compatible (forward-compat is mandatory for core profiles on macOS).
- The renderer uses the **drawable** size (`SDL_GL_GetDrawableSize`) for
  `glViewport`, and the **logical** window size for mouse hit-testing, so the
  scene stays crisp on Retina while clicks still land on the right squares.

## 4. What could NOT be verified without a Mac

This driver was written and **build-and-run verified on Linux** (NVIDIA, GL
4.3 core context: models load, both splat scenes decode + upload, no
GL/epoxy/shader errors, clean 12 s run). The following are
**reasoned but untested on real Apple hardware** — treat them as the first
things to check on a Mac:

- **Whisper / voice on Apple Silicon.** whisper.cpp's `auto` backend enables
  Metal on macOS by default. `Makefile.sdl` links `-framework Accelerate` (used
  by ggml's CPU path) but does **not** add the Metal frameworks unless you build
  with `WHISPER_BACKEND=metal`:
  ```bash
  make -f Makefile.sdl WHISPER_BACKEND=metal -j8
  ```
  If the link fails with undefined Metal symbols, that's the fix. (CPU-only is
  also fine: `cmake`-build whisper without Metal.)
- **Piper neural-TTS (move announcer).** The prebuilt Piper binary the Makefile
  fetches is **Linux x86_64 only**. On macOS the rest of the app builds and runs;
  the "Speak moves" TTS just won't spawn (it fails gracefully at runtime). Supply
  a macOS `piper` binary at `third_party/piper-bin/piper/piper` to enable it.
- **Chessnut Move BLE bridge.** Wired to CoreBluetooth via SimpleBLE; the link
  flags are in place but the connect/notify handshake hasn't been exercised on
  macOS Bluetooth.
- **Retina / HiDPI.** The logical-vs-drawable split is implemented per Apple's
  model but only tested on a 1× Linux display where the two sizes coincide.
- **SDL `main` shim.** We use `SDL_MAIN_HANDLED` + `SDL_SetMainReady()` to drive
  our own `main()` (no `-lSDL2main`, no Cocoa main wrapper). Modern SDL2
  (≥ 2.0.18) sets up the NSApplication inside `SDL_Init` on the main thread, so
  this is expected to work, but it wasn't run through a Mac windowing session.
