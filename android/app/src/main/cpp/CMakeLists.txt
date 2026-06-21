# ===========================================================================
# android/app/src/main/cpp/CMakeLists.txt — NDK build of libmain.so for 3D Chess
# ===========================================================================
#
# Invoked by Gradle's externalNativeBuild (app/build.gradle). Produces the
# shared library "libmain.so" that SDL2's Java SDLActivity loads and whose
# SDL_main it calls. Builds the SAME shared C++ engine the web/iOS builds
# compile (minus GTK/desktop-only TUs), the stb_truetype font baker, the
# Android SDL driver, and the placeholder AI / platform stubs.
#
# THIS HAS NOT BEEN BUILD-VERIFIED — Android requires the SDK + NDK + Gradle,
# which the authoring environment does not have. Treat it as a well-formed
# scaffold to open in Android Studio and finish on a machine with the tooling;
# see docs/ANDROID.md (the #1 TODO is replacing the placeholder AI with an
# in-process Stockfish).
# ===========================================================================

cmake_minimum_required(VERSION 3.22.1)

project(threedchess LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repo root: this file lives at android/app/src/main/cpp/. Five levels up.
get_filename_component(REPO_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)

# ===========================================================================
# SDL2 for Android.
# ===========================================================================
# The standard SDL2-Android layout: vendor SDL2's source tree next to this file
# (android/app/src/main/cpp/SDL) — a git submodule or symlink — and build it as
# a CMake subproject. SDL2's own CMake produces libSDL2.so + libhidapi.so for
# the NDK, and its Java glue (org.libsdl.app.*) is pulled in by app/build.gradle
# (sourceSets java.srcDirs → SDL/android-project/app/src/main/java). See
# docs/ANDROID.md for the exact `git submodule add` command and the AAR/prefab
# alternative.
set(SDL2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SDL" CACHE PATH "Vendored SDL2 source")
if(EXISTS "${SDL2_DIR}/CMakeLists.txt")
    add_subdirectory("${SDL2_DIR}" SDL2-build)
else()
    message(FATAL_ERROR
        "SDL2 source not found at ${SDL2_DIR}.\n"
        "Vendor it once, e.g.:\n"
        "  git submodule add -b release-2.30.x https://github.com/libsdl-org/SDL \\\n"
        "      android/app/src/main/cpp/SDL\n"
        "or drop a checkout there. See docs/ANDROID.md.")
endif()

# ===========================================================================
# Sources.
# ===========================================================================
# Shared engine TUs — IDENTICAL to the web/iOS builds' shared list, because
# Android mirrors them: the same platform-agnostic layer, minus the desktop-only
# TUs (gl_raster/, voice_whisper, voice_tts_native, chessnut_bridge,
# phantom_bridge) and minus every main_*.cpp.
set(SHARED_SRCS
    chess_types.cpp chess_rules.cpp game_state.cpp app_state.cpp
    board_renderer.cpp cloth_flag.cpp vec.cpp mat.cpp stl_model.cpp shader.cpp
    challenge.cpp ai_player.cpp time_control.cpp audio.cpp compression.cpp
    menu_physics.cpp menu_input.cpp challenge_ui.cpp pregame_ui.cpp
    shatter_transition.cpp text_atlas.cpp options_ui.cpp
    voice_input.cpp voice_tts.cpp puzzle.cpp openings.cpp endgame.cpp
    tactics.cpp pawn_structure.cpp opening_plans.cpp splat.cpp packed_splats.cpp)
list(TRANSFORM SHARED_SRCS PREPEND "${REPO_ROOT}/")

# stb_truetype text baker — shared with the web/iOS builds (platform-agnostic).
list(APPEND SHARED_SRCS "${REPO_ROOT}/web/font_atlas_stb.cpp")

# Android-only TUs (this directory): driver, placeholder AI, platform stubs.
set(ANDROID_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/main_android.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ai_player_android.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform_android.cpp")

# ===========================================================================
# The shared library SDLActivity loads. MUST be named "main" → libmain.so
# (SDLActivity's default; override with SDLActivity.getMainSharedObject()).
# ===========================================================================
add_library(main SHARED ${SHARED_SRCS} ${ANDROID_SRCS})

target_include_directories(main PRIVATE
    "${REPO_ROOT}"        # engine headers (app_state.h, board_renderer.h, …)
    "${REPO_ROOT}/web")   # stb_truetype.h lives in web/

# CHESS_PLATFORM_ANDROID routes app_state.cpp onto the web-style stub paths
# (no whisper / SimpleBLE / fork-exec) via the CHESS_PLATFORM_MOBILE superset.
# AI_PLAYER_HELPERS_ONLY strips the subprocess StockfishEngine out of
# ai_player.cpp, leaving only the shared FEN/UCI helpers — exactly as the
# web/iOS builds do. The GLES header (CHESS_GLES) + stb text (CHESS_TEXT_STB)
# selection in the shared code keys off __ANDROID__, which the NDK sets
# automatically.
target_compile_definitions(main PRIVATE
    CHESS_PLATFORM_ANDROID=1
    AI_PLAYER_HELPERS_ONLY=1)

# ---------------------------------------------------------------------------
# Link: SDL2 + the Android system libs the renderer/SDL need. zlib (z) ships
# with the NDK (compression.cpp / packed_splats.cpp). GLESv3 + EGL for the GL
# context; android + log for the platform + logcat.
# ---------------------------------------------------------------------------
target_link_libraries(main PRIVATE
    SDL2::SDL2
    GLESv3
    EGL
    android
    log
    z)
