#pragma once

#include "chess_types.h"
#include "stl_model.h"

#include <vector>

// Tumbling chess piece that lives on the main-menu backdrop. Kept in
// world space; the renderer reads x/y/z + rot_x/y/z + scale, the
// physics in menu_update_physics advances them, and the menu input
// handlers (ray pick, fling) mutate velocity/spin directly.
struct PhysicsPiece {
    PieceType type;
    float x, y, z;
    float vx, vy, vz;
    float rot_x, rot_y, rot_z;
    float spin_x, spin_y, spin_z;
    float scale;
};

// Spawn the 12 pieces that rain down on the menu. Seeded from the
// wall clock so each menu entry looks different.
void menu_init_physics(std::vector<PhysicsPiece>& pieces);

// Advance physics by dt: gravity + ballistic motion + spin,
// rotated-AABB boundary bounce, and OBB-vs-OBB piece-piece collision
// over each piece's per-slice collision sub-boxes.
void menu_update_physics(std::vector<PhysicsPiece>& pieces, float dt);

// Build per-piece collision geometry (world AABB half-extents +
// vertical sub-box stack) from the loaded meshes. Called once from
// renderer_init after the STL data is available; safe to skip on
// headless builds where physics never runs.
void menu_physics_init(StlModel loaded_models[PIECE_COUNT]);

// World-space AABB half-extents for a tumbling piece. Public so the
// menu ray-pick in board_renderer.cpp can reuse the same bounds for
// cursor hit-testing. out[0..2] = |half extent along world X/Y/Z|.
void menu_piece_world_half_extents(const PhysicsPiece& p, float out[3]);
