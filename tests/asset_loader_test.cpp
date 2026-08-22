// Priority-tiered asset loading: which groups gate a game start, how
// progress aggregates, and — the property the UI depends on — that a
// failed download degrades the scene instead of wedging the game.

#include "doctest.h"
#include "helpers.h"

#include "../asset_loader.h"

namespace {
// Settle every group a game needs except the named one.
void settle_all_but(AssetGroup keep_pending, int env, bool splats) {
    for (int i = 0; i < ASSET_GROUP_COUNT; i++) {
        AssetGroup g = static_cast<AssetGroup>(i);
        if (g == keep_pending) continue;
        if (!assets_group_needed_for_game(g, env, splats)) continue;
        assets_set_state(g, ASSET_READY);
    }
}
}  // namespace

TEST_CASE("core is ready as soon as the module starts") {
    assets_reset();
    // Core rides inside chess.data, which Emscripten mounts before any
    // of our code runs — so it is never pending.
    CHECK(assets_state(ASSET_CORE) == ASSET_READY);
    CHECK(assets_settled(ASSET_CORE));
    // Everything else starts out unrequested.
    CHECK(assets_state(ASSET_GAME) == ASSET_PENDING);
    CHECK(assets_state(ASSET_RETRO) == ASSET_PENDING);
    CHECK_FALSE(assets_settled(ASSET_GAME));
}

TEST_CASE("desktop marks every group ready") {
    assets_reset();
    assets_mark_all_ready();
    for (int i = 0; i < ASSET_GROUP_COUNT; i++)
        CHECK(assets_settled(static_cast<AssetGroup>(i)));
    CHECK(assets_ready_for_game(0, true));
    CHECK(assets_ready_for_game(1, true));
    CHECK(assets_game_progress(0, true) == doctest::Approx(1.0));
}

TEST_CASE("only the selected environment's heavy assets gate a start") {
    assets_reset();

    // Medieval room (env 0) with splats on: needs the medieval backdrop
    // but neither the retro set nor the data-centre cloud.
    CHECK(assets_group_needed_for_game(ASSET_GAME, 0, true));
    CHECK(assets_group_needed_for_game(ASSET_SPLAT_MEDIEVAL, 0, true));
    CHECK_FALSE(assets_group_needed_for_game(ASSET_RETRO, 0, true));
    CHECK_FALSE(assets_group_needed_for_game(ASSET_SPLAT_DATACENTER, 0, true));

    // Cable Room (env 1) needs the retro pieces and its own backdrop.
    CHECK(assets_group_needed_for_game(ASSET_RETRO, 1, true));
    CHECK(assets_group_needed_for_game(ASSET_SPLAT_DATACENTER, 1, true));
    CHECK_FALSE(assets_group_needed_for_game(ASSET_SPLAT_MEDIEVAL, 1, true));

    // Splats off: no backdrop is required at all, but the retro set is
    // still the piece geometry for env 1.
    CHECK_FALSE(assets_group_needed_for_game(ASSET_SPLAT_MEDIEVAL, 0, false));
    CHECK_FALSE(assets_group_needed_for_game(ASSET_SPLAT_DATACENTER, 1, false));
    CHECK(assets_group_needed_for_game(ASSET_RETRO, 1, false));

    // Puzzles are their own screen — never a game-start dependency.
    CHECK_FALSE(assets_group_needed_for_game(ASSET_PUZZLES, 0, true));
    CHECK_FALSE(assets_group_needed_for_game(ASSET_PUZZLES, 1, true));
}

TEST_CASE("a game waits for exactly the groups it needs") {
    assets_reset();
    CHECK_FALSE(assets_ready_for_game(0, true));

    settle_all_but(ASSET_SPLAT_MEDIEVAL, 0, true);
    CHECK_FALSE(assets_ready_for_game(0, true));      // backdrop outstanding
    CHECK(assets_first_pending_for_game(0, true) == ASSET_SPLAT_MEDIEVAL);

    assets_set_state(ASSET_SPLAT_MEDIEVAL, ASSET_READY);
    CHECK(assets_ready_for_game(0, true));
    CHECK(assets_first_pending_for_game(0, true) == ASSET_GROUP_COUNT);

    // The same state does not satisfy the Cable Room: its retro set and
    // backdrop are still pending.
    CHECK_FALSE(assets_ready_for_game(1, true));

    // ...but turning splats off drops the backdrop requirement, so only
    // the retro set stands between us and a Cable Room game.
    assets_set_state(ASSET_RETRO, ASSET_READY);
    CHECK(assets_ready_for_game(1, false));
    CHECK_FALSE(assets_ready_for_game(1, true));
}

TEST_CASE("a failed download degrades instead of blocking forever") {
    assets_reset();
    settle_all_but(ASSET_SPLAT_MEDIEVAL, 0, true);
    CHECK_FALSE(assets_ready_for_game(0, true));

    // The renderer already falls back (plain clear with no splat, STL
    // pieces with no retro set), so a dead download must let play start.
    assets_set_state(ASSET_SPLAT_MEDIEVAL, ASSET_FAILED);
    CHECK(assets_settled(ASSET_SPLAT_MEDIEVAL));
    CHECK(assets_ready_for_game(0, true));
    CHECK(assets_game_progress(0, true) == doctest::Approx(1.0));
}

TEST_CASE("progress aggregates over the needed groups only") {
    assets_reset();
    assets_set_state(ASSET_GAME, ASSET_LOADING);
    assets_set_progress(ASSET_GAME, 50, 100);
    assets_set_state(ASSET_SPLAT_MEDIEVAL, ASSET_LOADING);
    assets_set_progress(ASSET_SPLAT_MEDIEVAL, 0, 100);

    CHECK(assets_group_progress(ASSET_GAME) == doctest::Approx(0.5));

    // env 0 + splats needs Core(1.0) + Game(0.5) + Medieval(0.0).
    CHECK(assets_game_progress(0, true) == doctest::Approx(0.5));
    // With splats off the backdrop drops out: Core(1.0) + Game(0.5).
    CHECK(assets_game_progress(0, false) == doctest::Approx(0.75));

    // A group whose length is not known yet reads as 0, not NaN.
    assets_set_progress(ASSET_GAME, 10, 0);
    CHECK(assets_group_progress(ASSET_GAME) == doctest::Approx(0.0));

    // Settling wins over whatever the byte counters last said.
    assets_set_state(ASSET_GAME, ASSET_READY);
    CHECK(assets_group_progress(ASSET_GAME) == doctest::Approx(1.0));
}

TEST_CASE("downloads are ordered by the active environment") {
    assets_reset();
    // Whatever the scene, the shared game tier is fetched first.
    CHECK(assets_next_download(0) == ASSET_GAME);
    assets_set_state(ASSET_GAME, ASSET_READY);

    // Medieval room: its own backdrop before the Cable Room's assets.
    CHECK(assets_next_download(0) == ASSET_SPLAT_MEDIEVAL);

    // A player whose saved scene is the Cable Room gets the retro set
    // and the data-centre cloud first instead.
    CHECK(assets_next_download(1) == ASSET_RETRO);
    assets_set_state(ASSET_RETRO, ASSET_READY);
    CHECK(assets_next_download(1) == ASSET_SPLAT_DATACENTER);

    // The puzzle library is last — it never gates a game.
    assets_set_state(ASSET_SPLAT_MEDIEVAL, ASSET_READY);
    assets_set_state(ASSET_SPLAT_DATACENTER, ASSET_READY);
    CHECK(assets_next_download(0) == ASSET_PUZZLES);

    // Once everything is requested there is nothing left to queue.
    assets_set_state(ASSET_PUZZLES, ASSET_READY);
    CHECK(assets_next_download(0) == ASSET_GROUP_COUNT);
    CHECK(assets_next_download(1) == ASSET_GROUP_COUNT);
}

TEST_CASE("a download in flight is not queued twice") {
    assets_reset();
    assets_set_state(ASSET_GAME, ASSET_LOADING);
    // Loading != pending: the pump must move on rather than re-request.
    CHECK(assets_next_download(0) == ASSET_SPLAT_MEDIEVAL);
}

TEST_CASE("every group has a label for the loading UI") {
    for (int i = 0; i < ASSET_GROUP_COUNT; i++) {
        const char* s = assets_group_label(static_cast<AssetGroup>(i));
        REQUIRE(s != nullptr);
        CHECK(s[0] != '\0');
    }
}
