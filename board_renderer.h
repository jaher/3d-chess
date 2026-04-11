#pragma once

#include "chess_types.h"
#include "stl_model.h"

// Initialize GL resources (call from on_realize)
void renderer_init(StlModel loaded_models[PIECE_COUNT]);

// Main render function (call from on_render)
void renderer_draw(GameState& gs, int width, int height,
                   float rot_x, float rot_y, float zoom);

// Menu screen
struct PhysicsPiece {
    PieceType type;
    float x, y, z;
    float vx, vy, vz;
    float rot_x, rot_y, rot_z;
    float spin_x, spin_y, spin_z;
    float scale;
};

void menu_init_physics(std::vector<PhysicsPiece>& pieces);
void menu_update_physics(std::vector<PhysicsPiece>& pieces, float dt);
void renderer_draw_menu(const std::vector<PhysicsPiece>& pieces,
                        int width, int height, float time,
                        int hover_button); // 0=none, 1=start, 2=quit

// Returns 0=none, 1=start, 2=quit based on mouse position
int menu_hit_test(double mx, double my, int width, int height);
