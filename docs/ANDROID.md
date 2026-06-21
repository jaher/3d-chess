# Android build (experimental scaffold)

> **Status: scaffold only — NOT build-verified.**
> Android can only be built with the Android SDK + NDK + Gradle, which the
> authoring environment did not have (`which sdkmanager gradle ndk-build` →
> nothing; `$ANDROID_HOME` / `$ANDROID_NDK_HOME` unset). Everything under
> `android/` is a complete, documented starting point for a developer with the
> Android tooling to open and finish. **None of it has been compiled or run on
> a device/emulator.** Expect to iterate. The existing Linux GTK (`make chess`),
> web (`make -C web`), native SDL2 (`make -f Makefile.sdl`), iOS scaffold, and
> the 168-case test suite (`make -C tests test`) are all unaffected — every
> Android addition is additive or guarded.

Android is the **close sibling of the iOS scaffold** ([`docs/IOS.md`](IOS.md)),
which in turn is architecturally **closer to the web build than the desktop SDL
build**:

| Concern | Desktop (`main_sdl.cpp`) | Web (`web/main_web.cpp`) | iOS (`ios/main_ios.cpp`) | **Android (`android/.../main_android.cpp`)** |
|---|---|---|---|---|
| GL | desktop GL 4.1–4.3 (epoxy) | WebGL2 / GLES3 | OpenGL ES 3.0 (`<OpenGLES/ES3/gl.h>`) | **OpenGL ES 3.0** (`<GLES3/gl3.h>`, the NDK header) |
| Splat backdrop | GL-compute (Linux) / per-quad (macOS) | per-quad | per-quad | **per-quad** (no compute shaders) |
| Text | Cairo/Pango | stb_truetype | stb_truetype | **stb_truetype** (`web/font_atlas_stb.cpp`) |
| Touch | mouse | `SDL_FINGER*` (1=mouse, 2=pinch) | `SDL_FINGER*` | **`SDL_FINGER*`** (lifted from iOS/web) |
| AI | Stockfish **subprocess** | Stockfish.js Web Worker | **placeholder** | **placeholder** (no subprocess — see below) |
| Voice / Chessnut | whisper.cpp / SimpleBLE | browser feature-detect | unsupported | **unsupported** (feature-detect) |
| Assets | filesystem | emscripten preload VFS | `.app` bundle + `chdir` | **APK → extract to internal storage + `chdir`** |

## How the shared code stays one codebase

Android reuses the exact same platform-agnostic engine (`app_state.cpp`,
`board_renderer.cpp`, …) as every other target. Two small, guarded shared
macros (introduced/extended by this scaffold, behaviour-preserving for the
existing builds) wire Android in:

- **`CHESS_GLES`** — the OpenGL ES family discriminator (web **+ Android +**
  iOS), a superset of `__EMSCRIPTEN__`. Defined in the GL-header selection of
  `shader.h`, `board_renderer.h`, `render_internal.h`, `packed_splats.h`,
  `shatter_transition.cpp`, `text_atlas.cpp`. It picks `<GLES3/gl3.h>` on
  web + Android, `<OpenGLES/ES3/gl.h>` on iOS, and `epoxy` on
  Linux/macOS-desktop. The `__ANDROID__` branch is unreachable on every other
  build, so their object code is byte-for-byte unchanged.
- **`CHESS_TEXT_STB`** — bake glyphs with stb_truetype instead of Cairo/Pango.
  Android joins web + iOS on this path (`text_atlas.cpp`).
- **`CHESS_PLATFORM_ANDROID` / `CHESS_PLATFORM_MOBILE`** — in `app_state.cpp`,
  Android joins iOS on the web-style stub paths (no whisper / SimpleBLE /
  fork-exec). `CHESS_PLATFORM_MOBILE` (= iOS ∪ Android) is a superset of the
  iOS author's `CHESS_PLATFORM_IOS`, so the existing iOS guards keep behaving
  identically. The Android CMake also passes `-DCHESS_PLATFORM_ANDROID=1`; the
  NDK auto-defines `__ANDROID__`.

## Prerequisites

- **Android Studio** (Hedgehog / Iguana or newer), OR the command-line SDK:
  `sdkmanager` + `platform-tools` + a `platforms;android-34` + `build-tools`.
- The **Android NDK** (r26+ recommended; set `ndkVersion` in
  `app/build.gradle` to one you have installed via
  `sdkmanager "ndk;26.3.11579264"`).
- **CMake** ≥ 3.22 (the SDK's bundled CMake is fine — `sdkmanager "cmake;3.22.1"`).
- **JDK 17** (bundled with recent Android Studio).
- A `local.properties` at `android/local.properties` pointing at the SDK
  (Android Studio writes this for you):
  ```properties
  sdk.dir=/path/to/Android/Sdk
  ```

### Supplying SDL2 (pick one, then document your choice)

The app extends SDL2's `org.libsdl.app.SDLActivity` and links `libSDL2.so`, so
you must provide SDL2's **native source + Java glue**. The scaffold is wired for
the **vendored-source** approach (the standard SDL2-Android layout):

```bash
# from the repo root
git submodule add -b release-2.30.x https://github.com/libsdl-org/SDL \
    android/app/src/main/cpp/SDL
```

`app/src/main/cpp/CMakeLists.txt` then `add_subdirectory(SDL)` builds
`libSDL2.so`, and `app/build.gradle` adds
`SDL/android-project/app/src/main/java` to the Java source set so
`org.libsdl.app.*` (and thus `SDLActivity`) is on the classpath.

**Alternative:** use the **SDL2 prefab AAR**
(`implementation 'org.libsdl:SDL2:2.30.x'` + `buildFeatures { prefab true }` +
`find_package(SDL2)` in CMake). If you go this route, delete the `add_subdirectory(SDL)`
block and the `java.srcDirs += …/SDL/…` line, and add the dependency. The AAR
ships both the prefab native package and the Java glue.

### Materialize the Gradle wrapper jar

The binary `gradle/wrapper/gradle-wrapper.jar` is not committed (see
`gradle/wrapper/README.txt`). Generate it once — Android Studio does this on
first sync, or:

```bash
cd android && gradle wrapper --gradle-version 8.7
```

## Build

```bash
cd android

# Debug APK (debuggable, auto-signed with the debug keystore):
./gradlew assembleDebug
# → app/build/outputs/apk/debug/app-debug.apk

# Install + launch on a connected device / running emulator:
./gradlew installDebug
adb shell am start -n com.example.threedchess/.MainActivity
```

…or simply **open `android/` in Android Studio**, let it sync, pick a device,
and press Run.

The native `externalNativeBuild` compiles the shared engine + the Android
driver into `libmain.so` for the ABIs in `abiFilters` (default `arm64-v8a`; add
`x86_64` for the emulator).

## Rendering note: per-quad splats / no compute

Android OpenGL ES 3.0 has **no compute shaders**, so the GL-compute tile
rasterizer for the Gaussian-splat backdrop (`gl_raster/`, which needs desktop
GL 4.3) cannot run. `main_android.cpp` forces `CHESS_GL_COMPUTE_SPLATS=0` at
startup and `gl_raster/` is not compiled into the Android target — the splat
backdrop renders via the per-quad path, exactly like the web / iOS / macOS
builds. (GLES 3.1 *does* expose compute, but the scaffold deliberately does not
attempt it.)

## How assets resolve (APK → internal storage)

The shared engine loads assets by **relative path** (`models/…`, `fonts/…`,
`sounds/…`, `puzzles/…`, `openings/…`, `challenges/…`) using `std::ifstream` /
`fopen`. Those do **not** read from the Android **asset manager** — APK assets
are not real files. So the scaffold takes the simplest robust approach:

1. **Build time** — the Gradle `stageAssets` task copies the repo's
   `models/ sounds/ fonts/ challenges/ puzzles/ openings/` (+ optional `.spz`
   splats) into the APK's `assets/`, preserving sub-directory structure, and
   writes an `asset_manifest.txt` listing every bundled file. (Using a staged
   dir means the repo's asset directories are **not duplicated** into the source
   tree.)
2. **First run** — `main_android.cpp` reads `asset_manifest.txt` via
   `SDL_RWFromFile` (which **does** route to the asset manager for relative read
   paths on Android), extracts each listed file out of the APK into
   `SDL_AndroidGetInternalStoragePath()`, drops a `.assets_extracted_v1`
   sentinel, then `chdir()`s there. After that every relative path in the shared
   loaders resolves with **no change to the shared asset-loading code** — the
   same idea as the iOS `chdir(SDL_GetBasePath())`.

> **Known asset quirk to verify on-device:** `web/font_atlas_stb.cpp` opens the
> TTFs by the emscripten-VFS **absolute** path `"/fonts/Inter-Bold.ttf"` /
> `"/fonts/Cinzel-Bold.ttf"`. After `chdir()` to internal storage those absolute
> paths won't resolve on Android (nor on iOS — it's the same latent issue). Make
> the font path relative (`"fonts/…"`, which also works on web since emscripten's
> CWD is `/`) **or** special-case the mobile font directory. Until then the text
> baker falls back to a 1×1 white texture (glyphs won't render). This is the #4
> item below.

## Not yet done / needs Android tooling

None of the following is build-verified — the Android SDK/NDK + Gradle + a
device or emulator are required.

1. **★ In-process Stockfish (the big one).** An APK cannot `fork()`/`exec()` a
   bundled Stockfish binary under the app sandbox (and Play policy forbids it),
   so the desktop subprocess engine (`ai_player.cpp`'s `StockfishEngine`) cannot
   run. `android/.../ai_player_android.cpp` is a **placeholder** that plays a
   legal (lightly capture-biased) move so the app is runnable — it does **not**
   play strong chess, and evaluation/hints are disabled (`INT_MIN`). The real
   fix is to **build Stockfish as a static library with the NDK and drive UCI
   in-process** (a worker `std::thread` + queues, or in-process pipes) — the
   same approach embedded chess apps use. The FEN/UCI helpers are already shared
   (`ai_player.cpp` is compiled with `-DAI_PLAYER_HELPERS_ONLY`, like web/iOS),
   so only the `ask_ai_move` / `stockfish_eval` transport changes. This restores
   AI strength, the eval bar, hints, and move classification.
2. **Finish the renderer's GLES adaptation.** The bounded shared edits done so
   far cover the **GL-header selection** (now `CHESS_GLES`), the **stb text
   path** (`CHESS_TEXT_STB`), and the mobile stub guards. `board_renderer.cpp`
   still has ~40 `#ifndef __EMSCRIPTEN__` blocks that gate desktop-GL/compute
   code (MSAA FBO blits, `gl_raster` usage, glib timing). On Android those
   currently fall to the *desktop* branch and will not compile against GLES.
   **Recommended:** switch those GL-path guards from `__EMSCRIPTEN__` to the new
   `CHESS_GLES` (web ∪ Android ∪ iOS). Because `CHESS_GLES ⊇ __EMSCRIPTEN__`,
   that is behaviour-preserving for web/desktop (verify with `make -C web` and
   `make chess`). Keep the genuinely emscripten-API bits (`#include
   <emscripten.h>`, `emscripten_get_now`, `EM_ASM`) under `__EMSCRIPTEN__`. This
   is the **same** remaining work as iOS TODO #2 — do it once and both mobile
   targets benefit. Needs a GLES context (device/emulator) to verify and may
   surface OpenGL-ES-vs-WebGL2 API differences (extensions, precision, FBO
   formats).
3. **SDL2 Java glue wiring.** Vendor SDL2 (submodule) or switch to the prefab
   AAR (see *Supplying SDL2* above) and confirm `MainActivity extends
   SDLActivity` resolves and `libmain.so` loads with `SDL2` first.
4. **Asset-path verification on-device.** Confirm `stageAssets` packs the
   expected tree, the first-run extraction lands the files under
   `SDL_AndroidGetInternalStoragePath()`, and `chdir()` makes the relative loads
   work. **Fix the `/fonts/…` absolute-path quirk** (see the box above). If APK
   size matters, swap the desktop `models/` STLs for the packed
   `models-web-packed/` set the web build preloads.
5. **Touch tuning.** `PINCH_SENSITIVITY` and the one/two-finger gestures are
   copied verbatim from iOS/web; tune on-device for feel.
6. **App lifecycle.** `main_android.cpp` uses a plain `while` render loop. For
   correct background/foreground/suspend handling, handle the SDL `SDL_APP_*`
   lifecycle events (`SDL_APP_WILLENTERBACKGROUND` / `*DIDENTERFOREGROUND`) and
   pause rendering while backgrounded.
7. **Signing / keystore.** `release` uses no signing config. Add a release
   keystore (`signingConfigs { release { … } }`) and set a unique
   `applicationId` before distributing.
8. **Move-announcer TTS** → `android.speech.tts.TextToSpeech` (JNI), the Android
   analogue of the web build's `speechSynthesis`. Implement
   `voice_tts_init/speak/shutdown` in a JNI shim. Currently stubbed unsupported
   in `platform_android.cpp`.
9. **Voice move input** → `android.speech.SpeechRecognizer` (JNI) + mic capture,
   feeding `parse_voice_command()`. Stubbed unsupported. Needs the
   `RECORD_AUDIO` permission.
10. **Chessnut Move board** → Android BLE (`android.bluetooth.le`, JNI). The wire
    format is already shared (`chessnut_encode.h`); only the transport differs.
    Stubbed unsupported. Needs `BLUETOOTH_SCAN` / `BLUETOOTH_CONNECT` runtime
    permissions. (There is a decompiled reference Android Chessnut app noted in
    `docs/CHESSNUT.md`.)
11. **Puzzle fetch** is stubbed (delivers an empty body → the shared "couldn't
    load puzzle" hint). Wire a native `HttpURLConnection` (JNI) or cross-compile
    libcurl behind a `CHESS_ANDROID_ENABLE_CURL` flag.
12. **App icon / splash.** No launcher icon or splash is provided (uses the
    platform default). Add an icon set + a splash for a shippable build.

## File map

```
android/
  settings.gradle / build.gradle / gradle.properties  — Gradle project
  gradlew / gradlew.bat / gradle/wrapper/*            — wrapper (jar regenerated locally)
  app/
    build.gradle                                      — app module: minSdk 24,
                                                        externalNativeBuild(cmake),
                                                        abiFilters arm64-v8a,
                                                        stageAssets task, SDL java glue
    src/main/
      AndroidManifest.xml                             — SDLActivity, landscape,
                                                        GLES3 feature, fullscreen, INTERNET
      java/com/example/threedchess/MainActivity.java  — extends SDLActivity, loads SDL2 + main
      cpp/
        CMakeLists.txt                                — builds libmain.so from the shared
                                                        engine (= web/iOS list) + font_atlas_stb
                                                        + the 3 Android TUs; links SDL2,
                                                        GLESv3, EGL, android, log, z
        main_android.cpp                              — driver: GLES3 context, SDL_FINGER touch,
                                                        first-run APK asset extraction + chdir,
                                                        worker-thread AI dispatch
        ai_player_android.cpp                         — PLACEHOLDER AI (legal-move generator).
                                                        TODO: in-process Stockfish
        platform_android.cpp                          — voice / TTS / Chessnut "unsupported" stubs
        SDL/                                          — (you vendor this: SDL2 source submodule)
      assets/                                          — staged at build time by stageAssets
```

Shared-code touch points (all additive / guarded, inert on Linux/web/macOS):
the GL-header selection + `CHESS_GLES` in `board_renderer.h`, `shader.h`,
`render_internal.h`, `packed_splats.h`, `shatter_transition.cpp`,
`text_atlas.cpp`; the `CHESS_TEXT_STB` discriminator in `text_atlas.cpp`; and
`CHESS_PLATFORM_ANDROID` / `CHESS_PLATFORM_MOBILE` in `app_state.cpp`.
