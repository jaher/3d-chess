#pragma once

#include "menu_physics.h"  // PhysicsPiece

#include <vector>

// Menu-screen button layout in NDC. The hit-tests here and the
// overlay drawing in renderer_draw_menu share these so the click
// regions and the rendered backgrounds never drift apart.
//
// Six vertical slots are reserved (BTN_SLOT_1 = topmost). Layout
// depends on whether a Chessnut Move board is connected:
//
//   chessnut_connected = false        chessnut_connected = true
//   ─────────────────────────         ─────────────────────────
//   "Play against stockfish"          (subtitle suppressed)
//   SLOT_1: Start Game                SLOT_1: Multiplayer
//   SLOT_2: Puzzles                   SLOT_2: Start Game
//   SLOT_3: Homework (WIP)            SLOT_3: Puzzles
//   SLOT_4: Options                   SLOT_4: Homework (WIP)
//   SLOT_5: Quit                      SLOT_5: Options
//   (SLOT_6 unused)                   SLOT_6: Quit
//
// i.e. when Multiplayer is shown it takes the *Start Game* slot and
// every button below shifts one slot down — the buttons never crowd
// the "3D Chess" title.
namespace menu_ui {
constexpr float BTN_W       =  0.35f;
constexpr float BTN_H       =  0.08f;
constexpr float BTN_X       = -BTN_W * 0.5f;
constexpr float BTN_SLOT_1  =  0.12f;
constexpr float BTN_SLOT_2  = -0.05f;
constexpr float BTN_SLOT_3  = -0.22f;
constexpr float BTN_SLOT_4  = -0.39f;
constexpr float BTN_SLOT_5  = -0.56f;
constexpr float BTN_SLOT_6  = -0.73f;

// Per-button Y resolvers. `mp` = chessnut_connected (true → the
// Multiplayer button is on screen, all other buttons shift down).
constexpr float btn_multiplayer_y()              { return BTN_SLOT_1; }
constexpr float btn_start_y(bool mp)     { return mp ? BTN_SLOT_2 : BTN_SLOT_1; }
constexpr float btn_puzzle_y(bool mp)    { return mp ? BTN_SLOT_3 : BTN_SLOT_2; }
constexpr float btn_challenge_y(bool mp) { return mp ? BTN_SLOT_4 : BTN_SLOT_3; }
constexpr float btn_options_y(bool mp)   { return mp ? BTN_SLOT_5 : BTN_SLOT_4; }
constexpr float btn_quit_y(bool mp)      { return mp ? BTN_SLOT_6 : BTN_SLOT_5; }
}  // namespace menu_ui

// Returns 0=none, 1=start, 2=quit, 3=challenges, 4=options,
// 5=multiplayer (only when chessnut_connected is true), 6=puzzles.
// (Code 3 is labelled "Homework (WIP)" in the UI but kept its
// internal name to avoid churning the dispatch + voice-command
// wiring for what is just a renamed button.)
int menu_hit_test(double mx, double my, int width, int height,
                  bool chessnut_connected);

// Ray-pick a menu piece under the cursor. Returns the index of the
// nearest-hit piece (the one closest to the camera), or -1 on miss.
// `time` must match the value passed to renderer_draw_menu on the
// same frame so the camera transform lines up with what's on screen.
int menu_piece_hit_test(const std::vector<PhysicsPiece>& pieces,
                        double mx, double my, int width, int height,
                        float time);

// Apply a drag-to-fling impulse to a menu piece. The cursor delta
// from press to release is unprojected at the piece's depth, giving
// a world-space velocity proportional to how fast the user flicked
// the cursor. Spin magnitude scales with throw speed.
void menu_throw_piece(PhysicsPiece& p,
                      double press_mx, double press_my,
                      double release_mx, double release_my,
                      float dt,
                      int width, int height, float time);
