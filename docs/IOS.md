# iOS / iPadOS build (experimental scaffold)

> **Status: scaffold only — NOT build-verified.**
> iOS can only be built on a Mac with Xcode, which the authoring environment
> did not have. Everything under `ios/` is a complete, documented starting
> point for a Mac developer to open and finish. **None of it has been compiled
> or run on Apple hardware.** Expect to iterate. The existing Linux GTK
> (`make chess`), web (`make -C web`), and native SDL2 (`make -f Makefile.sdl`)
> builds are unaffected — every iOS addition is additive or guarded.

The iOS port is architecturally **closer to the web build than the desktop SDL
build**:

| Concern | Desktop (`main_sdl.cpp`) | Web (`web/main_web.cpp`) | **iOS (`ios/main_ios.cpp`)** |
|---|---|---|---|
| GL | desktop GL 4.1–4.3 (epoxy) | WebGL2 / GLES3 | **OpenGL ES 3.0** (`<OpenGLES/ES3/gl.h>`) |
| Gaussian-splat backdrop | GL-compute tile rasterizer (Linux) / per-quad (macOS) | per-quad | **per-quad** (no compute shaders) |
| Text | Cairo/Pango | stb_truetype | **stb_truetype** (`web/font_atlas_stb.cpp`) |
| Touch | mouse | `SDL_FINGER*` (1 = mouse, 2 = pinch) | **`SDL_FINGER*`** (lifted from web) |
| AI | Stockfish **subprocess** | Stockfish.js Web Worker | **placeholder** (no subprocess — see below) |
| Voice / Chessnut | whisper.cpp / SimpleBLE | browser feature-detect | **unsupported** (feature-detect) |

## Prerequisites

- A **Mac** (Apple Silicon or Intel).
- **Xcode** 14+ (with the iOS SDK and Command Line Tools).
- **CMake** ≥ 3.20 (`brew install cmake`).
- For deploying to a **physical device**: a (free or paid) **Apple Developer
  account** and a 10-character **Team ID** for code signing. The Simulator
  needs no account.

SDL2 is fetched and built from source automatically (CMake `FetchContent`,
pinned to a release tag) — no Homebrew SDL needed. (If you prefer, drop a
prebuilt `SDL2.framework` beside `ios/CMakeLists.txt` and switch to
`find_package(SDL2)`.)

## Build

Generate an Xcode project from the repo root:

```bash
cmake -S ios -B build-ios -G Xcode \
      -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DBUNDLE_ID=com.yourorg.threedchess \
      -DIOS_DEPLOYMENT_TARGET=13.0
```

Then either open the project in Xcode:

```bash
open build-ios/3dchess_ios.xcodeproj
```

…or build from the command line:

```bash
# Simulator (no signing required):
cmake --build build-ios --config Debug -- \
      -sdk iphonesimulator -destination 'generic/platform=iOS Simulator'

# Device (requires a signing team):
cmake --build build-ios --config Debug -- \
      -sdk iphoneos -destination 'generic/platform=iOS' \
      DEVELOPMENT_TEAM=XXXXXXXXXX
```

Pass your Team ID once at configure time instead, if you prefer:

```bash
cmake -S ios -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DBUNDLE_ID=com.yourorg.threedchess \
      -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=XXXXXXXXXX
```

### Run on Simulator vs device

- **Simulator:** select an iPad/iPhone simulator in Xcode and Run, or
  `xcrun simctl`. The Simulator runs the GLES path fine; performance is not
  representative.
- **Device:** plug in, select it as the run destination, ensure the bundle ID
  + signing team are set (Signing & Capabilities tab), and Run.

## Rendering note: per-quad splats / no compute

iOS OpenGL ES 3.0 has **no compute shaders**, so the GL-compute tile
rasterizer for the Gaussian-splat backdrop (`gl_raster/`, which needs desktop
GL 4.3) cannot run. `ios/main_ios.cpp` forces `CHESS_GL_COMPUTE_SPLATS=0` at
startup and `gl_raster` is not even compiled into the iOS target — the splat
backdrop renders via the per-quad path, exactly like the web and macOS-desktop
builds. The board, pieces, PBR, shadows, and MSAA are otherwise the same engine.

## How the asset paths resolve

The shared engine loads assets by **relative** path (`models/…`, `fonts/…`,
`sounds/…`, `puzzles/…`, `openings/…`, `challenges/…`). On iOS the process
working directory is not the bundle, so `main_ios.cpp` calls
`chdir(SDL_GetBasePath())` at launch — `SDL_GetBasePath()` returns the `.app`
resource root — so every relative path resolves against the bundled
`RESOURCES`. `ios/CMakeLists.txt` bundles each asset directory while preserving
its sub-structure (e.g. `models/board/`). **Verify this end-to-end on a real
device/Simulator** (listed below).

---

## Not yet done / needs a Mac

None of the following is build-verified — a Mac + Xcode (and for some, a
device) is required.

1. **★ In-process Stockfish (the big one).** iOS forbids `fork()`/`exec()`, so
   the desktop subprocess engine (`ai_player.cpp`'s `StockfishEngine`) cannot
   run. `ios/ai_player_ios.cpp` is a **placeholder** that plays a legal (lightly
   capture-biased) move so the app is runnable — it does **not** play strong
   chess, and evaluation/hints are disabled (`INT_MIN`). The real fix is to
   **link Stockfish as a static library and drive UCI in-process** (a worker
   `std::thread` + queues, or in-process pipes) — the same approach embedded
   chess apps use. The FEN/UCI helpers are already shared (`ai_player.cpp` is
   compiled with `-DAI_PLAYER_HELPERS_ONLY`, like the web build), so only the
   `ask_ai_move` / `stockfish_eval` transport changes. This restores AI
   strength, the eval bar, hints, and move classification.
2. **Finish the renderer's GLES adaptation.** The bounded shared edits done so
   far cover the **GL-header selection**, the **stb text path**, and the
   **`gl_raster` include** (all guarded by `defined(__APPLE__) &&
   TARGET_OS_IPHONE`, inert on the other builds). `board_renderer.cpp` still
   has ~40 other `#ifndef __EMSCRIPTEN__` blocks that gate desktop-GL/compute
   code (MSAA FBO blits, `gl_raster` usage, glib timing). On iOS those
   currently fall to the *desktop* branch and will not compile against GLES.
   **The `CHESS_GLES` macro now exists** — the Android scaffold introduced it
   for the GL-header selection (defined for web **+ Android +** iOS, a superset
   of `__EMSCRIPTEN__`; see `shader.h` / `render_internal.h` / `board_renderer.h`
   / `packed_splats.h` / `shatter_transition.cpp` / `text_atlas.cpp`). The
   remaining work is to **switch those ~40 `#ifndef __EMSCRIPTEN__` GL-path
   guards in `board_renderer.cpp` to `CHESS_GLES`**. Because
   `CHESS_GLES ⊇ __EMSCRIPTEN__`, that change is behaviour-preserving for the
   existing web/desktop builds (verify with `make -C web` and `make chess`).
   Keep the genuinely emscripten-API bits (`#include <emscripten.h>`,
   `emscripten_get_now`, `EM_ASM`) under `__EMSCRIPTEN__`. This is the **same**
   remaining work as Android — do it once and both mobile targets benefit. It
   needs a Mac/Simulator (or Android device/emulator) GLES context to verify and
   may surface OpenGL-ES-vs-WebGL2 API differences (extensions, precision, FBO
   formats).
3. **Code signing.** Set your Apple Developer **Team ID**
   (`-DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=…` or Xcode's Signing tab) and a
   unique **bundle ID** (`-DBUNDLE_ID=…`). Automatic signing is pre-selected.
4. **Asset-path verification.** Confirm `SDL_GetBasePath()` + `chdir()` lands
   on the bundled `models/ fonts/ sounds/ puzzles/ openings/ challenges/`
   layout on a real device/Simulator, and that the per-file
   `MACOSX_PACKAGE_LOCATION` bundling preserved subdirectories. The piece STLs
   currently come from the desktop `models/` dir; if bundle size matters, swap
   to the packed `models-web-packed/` set the web build uses.
5. **Touch tuning.** `PINCH_SENSITIVITY` and the one/two-finger gestures are
   copied verbatim from the web build; tune on-device for feel.
6. **App lifecycle.** `main_ios.cpp` uses a plain `while` render loop. For
   correct background/foreground/suspend handling, switch to
   `SDL_iPhoneSetAnimationCallback` (display-link driven) and handle
   `SDL_APP_WILLENTERBACKGROUND` / `*DIDBECOMEACTIVE`.
7. **Move-announcer TTS** → `AVSpeechSynthesizer` (the iOS analogue of the
   web build's `speechSynthesis`); implement `voice_tts_init/speak/shutdown`
   in an Objective-C++ `.mm`. Currently stubbed unsupported in
   `ios/platform_ios.cpp`.
8. **Voice move input** → on-device `SFSpeechRecognizer` (Speech.framework) +
   `AVAudioEngine` mic capture, feeding `parse_voice_command()`. Stubbed
   unsupported. (The `NSMicrophoneUsageDescription` /
   `NSSpeechRecognitionUsageDescription` plist keys are pre-seeded.)
9. **Chessnut Move board** → `CoreBluetooth`. The wire format is already shared
   (`chessnut_encode.h`); only the BLE transport differs. Stubbed unsupported.
   (`NSBluetoothAlwaysUsageDescription` is pre-seeded.)
10. **Puzzle fetch** is stubbed (delivers an empty body → the shared "couldn't
    load puzzle" hint). libcurl works on iOS — wire it behind
    `CHESS_IOS_ENABLE_CURL` (port `main_sdl.cpp::fetch_url`) or use a native
    `NSURLSession` fetch.
11. **Launch screen / icons.** `Info.plist.in` uses an empty `UILaunchScreen`
    (black launch, full native resolution). Add a LaunchScreen storyboard and
    an app icon set for a shippable build.

## File map

```
ios/
  CMakeLists.txt     — iOS Xcode project: SDL2 (FetchContent), shared engine
                       sources (= web's list), iOS defines, asset bundling,
                       frameworks, signing knobs.
  Info.plist.in      — bundle id / name / landscape / status-bar-hidden /
                       ProMotion / usage strings (CMake-configured).
  main_ios.cpp       — driver: GLES3 context, fullscreen, SDL_FINGER touch,
                       bundle chdir, worker-thread AI dispatch.
  ai_player_ios.cpp  — PLACEHOLDER AI (legal-move generator). TODO: in-process
                       Stockfish.
  platform_ios.cpp   — voice / TTS / Chessnut "unsupported" stubs (mirror the
                       web feature-detect).
```

Shared-code touch points (all additive / guarded, inert on Linux/web/macOS):
`board_renderer.cpp`, `board_renderer.h`, `shader.h`, `render_internal.h`,
`packed_splats.h`, `shatter_transition.cpp`, `text_atlas.cpp` (GL-header + stb
text path), and `app_state.cpp` (`CHESS_PLATFORM_IOS` → web-style stub paths).
