# ===========================================================================
# ios/CMakeLists.txt — iOS / iPadOS app target for 3D Chess.
# ===========================================================================
#
# This produces an Xcode project that builds the shared C++ engine (the same
# translation units the web build compiles) against SDL2's iOS backend with
# an OpenGL ES 3.0 context, bundling all runtime assets into the .app.
#
# THIS HAS NOT BEEN BUILD-VERIFIED — iOS requires a Mac + Xcode, which the
# authoring environment does not have. Treat it as a well-formed scaffold to
# open and finish on a Mac; see docs/IOS.md for the full TODO list (the #1
# item is replacing the placeholder AI with an in-process Stockfish).
#
# Generate + build (on a Mac):
#
#   cmake -S ios -B build-ios -G Xcode \
#         -DCMAKE_SYSTEM_NAME=iOS \
#         -DCMAKE_OSX_ARCHITECTURES=arm64 \
#         -DBUNDLE_ID=com.yourorg.threedchess \
#         -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=XXXXXXXXXX
#   open build-ios/3dchess_ios.xcodeproj          # or: cmake --build build-ios
#
# ===========================================================================

cmake_minimum_required(VERSION 3.20)

# SDL2's iOS backend is Objective-C; the engine is C++. font_atlas_stb +
# stb_truetype are C++. Enable all four languages.
project(3dchess_ios LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Configurable knobs ----------------------------------------------------
set(BUNDLE_ID "com.example.threedchess" CACHE STRING "App bundle identifier")
set(IOS_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum iOS version")
set(PRODUCT_NAME "3D Chess" CACHE STRING "Display / product name")

set(CMAKE_OSX_DEPLOYMENT_TARGET "${IOS_DEPLOYMENT_TARGET}" CACHE STRING "" FORCE)

# Repo root (this file lives in ios/). All shared sources are one level up.
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# ===========================================================================
# SDL2 for iOS.
# ===========================================================================
# Built from source via FetchContent — SDL2's own CMake supports iOS and
# ships the UIKit `main` shim (SDL2main). Pin the tag for reproducibility.
# (Alternatively, drop a prebuilt SDL2.framework next to this file and
# `find_package(SDL2)` instead — see docs/IOS.md.)
include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-2.30.9   # pin; bump deliberately
)
FetchContent_MakeAvailable(SDL2)

# ===========================================================================
# Sources.
# ===========================================================================
# Shared engine TUs — IDENTICAL to the web build's SHARED_SRCS (web/Makefile),
# because iOS mirrors web: the same platform-agnostic layer, minus the
# desktop-only TUs (gl_raster/, voice_whisper, voice_tts_native,
# chessnut_bridge, phantom_bridge) and minus every main_*.cpp.
set(SHARED_SRCS
    chess_types.cpp chess_rules.cpp game_state.cpp app_state.cpp
    board_renderer.cpp cloth_flag.cpp vec.cpp mat.cpp stl_model.cpp shader.cpp
    challenge.cpp ai_player.cpp time_control.cpp audio.cpp compression.cpp
    menu_physics.cpp menu_input.cpp challenge_ui.cpp pregame_ui.cpp
    shatter_transition.cpp text_atlas.cpp options_ui.cpp
    voice_input.cpp voice_tts.cpp puzzle.cpp openings.cpp endgame.cpp
    tactics.cpp pawn_structure.cpp opening_plans.cpp splat.cpp packed_splats.cpp)
list(TRANSFORM SHARED_SRCS PREPEND "${REPO_ROOT}/")

# stb_truetype text baker — shared with the web build (it's platform-agnostic).
list(APPEND SHARED_SRCS "${REPO_ROOT}/web/font_atlas_stb.cpp")

# iOS-only TUs (this directory): driver, placeholder AI, platform stubs.
set(IOS_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/main_ios.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ai_player_ios.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform_ios.cpp")

# ===========================================================================
# App target.
# ===========================================================================
add_executable(3dchess_ios MACOSX_BUNDLE ${SHARED_SRCS} ${IOS_SRCS})

target_include_directories(3dchess_ios PRIVATE
    "${REPO_ROOT}"
    "${REPO_ROOT}/web")   # stb_truetype.h lives in web/

# CHESS_PLATFORM_IOS routes app_state.cpp onto the web-style stub paths
# (no whisper / SimpleBLE / fork-exec). AI_PLAYER_HELPERS_ONLY strips the
# subprocess StockfishEngine out of ai_player.cpp, leaving only the shared
# FEN/UCI helpers — exactly as the web build does. The GLES header + stb text
# selection in the shared code keys off `defined(__APPLE__) && TARGET_OS_IPHONE`,
# which the iOS toolchain sets automatically.
target_compile_definitions(3dchess_ios PRIVATE
    CHESS_PLATFORM_IOS=1
    AI_PLAYER_HELPERS_ONLY=1)

# ---------------------------------------------------------------------------
# Link: SDL2 (+ its UIKit main shim) and the iOS system frameworks SDL2 and
# the renderer need. zlib (libz) is a system library on iOS (compression.cpp /
# packed_splats.cpp).
# ---------------------------------------------------------------------------
target_link_libraries(3dchess_ios PRIVATE
    SDL2::SDL2-static
    SDL2::SDL2main
    z
    "-framework OpenGLES"
    "-framework UIKit"
    "-framework Foundation"
    "-framework CoreGraphics"
    "-framework QuartzCore"
    "-framework AVFoundation"
    "-framework GameController"
    "-framework AudioToolbox"
    "-framework CoreAudio"
    "-framework CoreMotion"
    "-framework CoreHaptics"
    "-framework Metal")

# ===========================================================================
# Bundle metadata (Info.plist) + signing.
# ===========================================================================
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    @ONLY)

set_target_properties(3dchess_ios PROPERTIES
    OUTPUT_NAME "3DChess"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${BUNDLE_ID}"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${IOS_DEPLOYMENT_TARGET}"
    # iPhone (1) + iPad (2): a universal app.
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    # TODO(Mac dev): set your 10-char Apple Developer Team ID to sign for a
    # device. Pass -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=XXXXXXXXXX at
    # configure time, or set it in Xcode's Signing & Capabilities tab.
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic")

# ===========================================================================
# Bundle the runtime assets as app RESOURCES.
# ===========================================================================
# The shared layer loads assets with RELATIVE paths ("models/…", "fonts/…",
# "sounds/…", "puzzles/…", "openings/…", "challenges/…"); main_ios.cpp
# chdir()s to SDL_GetBasePath() (the .app resource root) at launch so they
# resolve against the bundle. To make that work the on-disk directory
# structure must be preserved INSIDE the bundle — hence the per-file
# MACOSX_PACKAGE_LOCATION below (a flat RESOURCE property would lose subdirs
# like models/board/).
#
# NOTE: Apple-hardware verification of the final in-bundle layout is on the
# docs/IOS.md "needs a Mac" list — confirm SDL_GetBasePath()+chdir() lands on
# these paths on a real device/Simulator.
function(bundle_asset_dir target srcdir bundle_prefix)
    if(NOT EXISTS "${srcdir}")
        message(WARNING "iOS asset dir missing, skipping: ${srcdir}")
        return()
    endif()
    file(GLOB_RECURSE _files RELATIVE "${srcdir}" "${srcdir}/*")
    foreach(_f ${_files})
        get_filename_component(_subdir "${_f}" DIRECTORY)
        target_sources(${target} PRIVATE "${srcdir}/${_f}")
        set_source_files_properties("${srcdir}/${_f}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "${bundle_prefix}/${_subdir}"
            HEADER_FILE_ONLY ON)   # data, not compiled
    endforeach()
endfunction()

# Pieces + board + clock + table all live under models/ (the renderer loads
# models/<piece>.stl, models/board, models/clock, models/table).
bundle_asset_dir(3dchess_ios "${REPO_ROOT}/models"     "models")
bundle_asset_dir(3dchess_ios "${REPO_ROOT}/sounds"     "sounds")
bundle_asset_dir(3dchess_ios "${REPO_ROOT}/fonts"      "fonts")
bundle_asset_dir(3dchess_ios "${REPO_ROOT}/challenges" "challenges")
bundle_asset_dir(3dchess_ios "${REPO_ROOT}/puzzles"    "puzzles")
bundle_asset_dir(3dchess_ios "${REPO_ROOT}/openings"   "openings")

# Optional Gaussian-splat backdrops (.spz). Bundled only if present — a clean
# checkout without them still builds (board_renderer falls back to the dark
# clear when no SPZ loads), exactly like the web build's optional preload.
foreach(_spz
        "world_labs/medieval_room/splat_500k.spz")
    if(EXISTS "${REPO_ROOT}/${_spz}")
        get_filename_component(_spz_dir "${_spz}" DIRECTORY)
        target_sources(3dchess_ios PRIVATE "${REPO_ROOT}/${_spz}")
        set_source_files_properties("${REPO_ROOT}/${_spz}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "${_spz_dir}"
            HEADER_FILE_ONLY ON)
    endif()
endforeach()
