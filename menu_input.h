#pragma once

#include "menu_physics.h"  // PhysicsPiece

#include <vector>

// Menu-screen button layout in NDC. The hit-tests here and the
// overlay drawing in renderer_draw_menu share these so the click
// regions and the rendered backgrounds never drift apart.
//
// BTN_MULTIPLAYER_Y sits where the "Play against stockfish"
// subtitle is when no Chessnut Move is connected. When the board
// is connected, the subtitle is suppressed and the Multiplayer
// button takes its slot — same NDC range, no layout shift for
// the buttons below.
namespace menu_ui {
constexpr float BTN_W            =  0.35f;
constexpr float BTN_H            =  0.08f;
constexpr float BTN_X            = -BTN_W * 0.5f;
constexpr float BTN_MULTIPLAYER_Y=  0.25f;
constexpr float BTN_START_Y      =  0.12f;
constexpr float BTN_CHALLENGE_Y  = -0.05f;
constexpr float BTN_OPTIONS_Y    = -0.22f;
constexpr float BTN_QUIT_Y       = -0.39f;
}  // namespace menu_ui

// Returns 0=none, 1=start, 2=quit, 3=challenges, 4=options,
// 5=multiplayer (only when chessnut_connected is true).
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
