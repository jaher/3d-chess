#pragma once

#include "chess_types.h"
#include "stl_model.h"

// Initialize GL resources (call from on_realize)
void renderer_init(StlModel loaded_models[PIECE_COUNT]);

// Main render function (call from on_render)
void renderer_draw(GameState& gs, int width, int height,
                   float rot_x, float rot_y, float zoom);
