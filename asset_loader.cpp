#include "asset_loader.h"

namespace {

struct GroupInfo {
    AssetState state = ASSET_PENDING;
    double received = 0;
    double total = 0;
};

GroupInfo g_groups[ASSET_GROUP_COUNT];

bool valid(AssetGroup g) { return g >= 0 && g < ASSET_GROUP_COUNT; }

}  // namespace

void assets_reset() {
    for (int i = 0; i < ASSET_GROUP_COUNT; i++) g_groups[i] = GroupInfo{};
    // Core lives inside chess.data, so by the time any code runs it is
    // already mounted — Emscripten gates startup on that package.
    g_groups[ASSET_CORE].state = ASSET_READY;
}

void assets_mark_all_ready() {
    for (int i = 0; i < ASSET_GROUP_COUNT; i++) {
        g_groups[i].state = ASSET_READY;
        g_groups[i].received = g_groups[i].total = 1;
    }
}

void assets_set_state(AssetGroup g, AssetState s) {
    if (!valid(g)) return;
    g_groups[g].state = s;
    // Settling with no byte counts (desktop, or a package with an
    // unknown length) still has to read as 100% to the progress UI.
    if (s == ASSET_READY || s == ASSET_FAILED) {
        if (g_groups[g].total <= 0) g_groups[g].total = 1;
        g_groups[g].received = g_groups[g].total;
    }
}

AssetState assets_state(AssetGroup g) {
    return valid(g) ? g_groups[g].state : ASSET_READY;
}

bool assets_settled(AssetGroup g) {
    AssetState s = assets_state(g);
    return s == ASSET_READY || s == ASSET_FAILED;
}

void assets_set_progress(AssetGroup g, double received, double total) {
    if (!valid(g)) return;
    g_groups[g].received = received < 0 ? 0 : received;
    g_groups[g].total = total < 0 ? 0 : total;
}

double assets_group_progress(AssetGroup g) {
    if (!valid(g)) return 1.0;
    const GroupInfo& gi = g_groups[g];
    if (gi.state == ASSET_READY || gi.state == ASSET_FAILED) return 1.0;
    if (gi.total <= 0) return 0.0;
    double p = gi.received / gi.total;
    return p < 0 ? 0 : (p > 1 ? 1 : p);
}

bool assets_group_needed_for_game(AssetGroup g, int environment,
                                  bool splats_enabled) {
    switch (g) {
        case ASSET_CORE:
        case ASSET_GAME:
            return true;
        // The retro PC piece set is only drawn in the Cable Room.
        case ASSET_RETRO:
            return environment == 1;
        // Backdrops are per-environment, and only when splats are on:
        // with them switched off the renderer never samples the cloud.
        case ASSET_SPLAT_MEDIEVAL:
            return splats_enabled && environment == 0;
        case ASSET_SPLAT_DATACENTER:
            return splats_enabled && environment == 1;
        // Puzzles are their own screen, never needed to start a game.
        case ASSET_PUZZLES:
        default:
            return false;
    }
}

bool assets_ready_for_game(int environment, bool splats_enabled) {
    for (int i = 0; i < ASSET_GROUP_COUNT; i++) {
        AssetGroup g = static_cast<AssetGroup>(i);
        if (!assets_group_needed_for_game(g, environment, splats_enabled))
            continue;
        if (!assets_settled(g)) return false;
    }
    return true;
}

double assets_game_progress(int environment, bool splats_enabled) {
    double sum = 0;
    int n = 0;
    for (int i = 0; i < ASSET_GROUP_COUNT; i++) {
        AssetGroup g = static_cast<AssetGroup>(i);
        if (!assets_group_needed_for_game(g, environment, splats_enabled))
            continue;
        sum += assets_group_progress(g);
        n++;
    }
    return n ? sum / n : 1.0;
}

AssetGroup assets_first_pending_for_game(int environment,
                                         bool splats_enabled) {
    for (int i = 0; i < ASSET_GROUP_COUNT; i++) {
        AssetGroup g = static_cast<AssetGroup>(i);
        if (!assets_group_needed_for_game(g, environment, splats_enabled))
            continue;
        if (!assets_settled(g)) return g;
    }
    return ASSET_GROUP_COUNT;
}

AssetGroup assets_next_download(int environment) {
    AssetGroup order[ASSET_GROUP_COUNT];
    int n = 0;
    // Everything a game needs regardless of scene.
    order[n++] = ASSET_GAME;
    // Then the active environment's own heavy assets, so that starting a
    // game in the saved scene unblocks as early as possible; the other
    // environment is prefetched afterwards for a fast Options switch.
    if (environment == 1) {
        order[n++] = ASSET_RETRO;
        order[n++] = ASSET_SPLAT_DATACENTER;
        order[n++] = ASSET_SPLAT_MEDIEVAL;
    } else {
        order[n++] = ASSET_SPLAT_MEDIEVAL;
        order[n++] = ASSET_RETRO;
        order[n++] = ASSET_SPLAT_DATACENTER;
    }
    order[n++] = ASSET_PUZZLES;
    for (int i = 0; i < n; i++)
        if (assets_state(order[i]) == ASSET_PENDING) return order[i];
    return ASSET_GROUP_COUNT;
}

const char* assets_group_label(AssetGroup g) {
    switch (g) {
        case ASSET_CORE:             return "Pieces";
        case ASSET_GAME:             return "Board, clock & sounds";
        case ASSET_SPLAT_MEDIEVAL:   return "Medieval room";
        case ASSET_RETRO:            return "Retro piece set";
        case ASSET_SPLAT_DATACENTER: return "Data centre";
        case ASSET_PUZZLES:          return "Puzzle library";
        default:                     return "Assets";
    }
}
