// Priority-tiered asset loading.
//
// The web build used to ship every asset inside a single ~103 MB
// `chess.data` preload, which Emscripten downloads *before* main()
// runs — so the main menu couldn't appear until the splat backdrops
// (58 MB) and the retro piece set (17 MB) had arrived, even though
// neither is used by the menu.
//
// Assets are now split into priority groups. Only ASSET_CORE (the six
// piece meshes + fonts, ~4.5 MB) stays in `chess.data`; every other
// group is a separate package fetched in the background once the menu
// is up. This header is the platform-agnostic bookkeeping: which group
// is where, how far along it is, and — the part the UI cares about —
// whether enough has arrived to start a game.
//
// Deliberately free of GL/SDL/emscripten so it links into the pure
// logic test binary (see tests/asset_loader_test.cpp).
#pragma once

#include <cstddef>

// Ordered by download priority: earlier groups are requested first.
enum AssetGroup {
    ASSET_CORE = 0,          // piece meshes + fonts (inside chess.data)
    ASSET_GAME,              // board + clock + table + sounds + openings
    ASSET_SPLAT_MEDIEVAL,    // environment 0 backdrop
    ASSET_RETRO,             // retro piece set (environment 1)
    ASSET_SPLAT_DATACENTER,  // environment 1 backdrop
    ASSET_PUZZLES,           // daily-puzzle library
    ASSET_GROUP_COUNT
};

enum AssetState {
    ASSET_PENDING = 0,  // not requested yet
    ASSET_LOADING,      // download in flight
    ASSET_READY,        // in the filesystem, safe to load
    ASSET_FAILED        // gave up — callers degrade rather than block
};

// Reset every group to Pending (used by tests and at startup).
void assets_reset();

// Desktop: every asset is already on local disk, so nothing is ever
// pending. Keeps the shared game-start gate a single code path.
void assets_mark_all_ready();

void       assets_set_state(AssetGroup g, AssetState s);
AssetState assets_state(AssetGroup g);

// "Settled" means the group will never change again: either it arrived
// or it failed. A failed group must not wedge the game forever — the
// renderer already falls back (STL pieces when retro is missing, plain
// clear when a splat is missing), so a failed download degrades the
// scene instead of blocking play.
bool assets_settled(AssetGroup g);

// Byte-level progress for the group currently downloading. `total` may
// be 0 while the response headers are still unknown.
void   assets_set_progress(AssetGroup g, double received, double total);
double assets_group_progress(AssetGroup g);   // 0..1

// Which groups a game needs before it can start, given the selected
// environment and whether splats are switched on.
bool assets_group_needed_for_game(AssetGroup g, int environment,
                                  bool splats_enabled);

// True once every group needed for this environment has settled.
bool assets_ready_for_game(int environment, bool splats_enabled);

// Mean progress across the groups a game still needs (0..1). Drives the
// "Loading…" bar shown when the player starts before assets arrive.
double assets_game_progress(int environment, bool splats_enabled);

// First still-unsettled group needed for a game, or ASSET_GROUP_COUNT
// when nothing is outstanding — labels the progress UI.
AssetGroup assets_first_pending_for_game(int environment,
                                         bool splats_enabled);

// Next group the background downloader should fetch, or
// ASSET_GROUP_COUNT when everything has been requested. Downloads run
// one at a time so the highest-priority group lands first: whatever the
// *saved* environment needs comes before the other environment's
// assets, and the puzzle library comes last.
AssetGroup assets_next_download(int environment);

// Human-readable group name ("Board & pieces", "Backdrop", …).
const char* assets_group_label(AssetGroup g);
