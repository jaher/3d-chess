#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <epoxy/gl.h>

enum PieceType { KING, QUEEN, BISHOP, KNIGHT, ROOK, PAWN, PIECE_COUNT };
enum GameMode { MODE_MENU, MODE_PLAYING, MODE_CHALLENGE_SELECT, MODE_CHALLENGE };

extern const char* piece_filenames[PIECE_COUNT];
extern const float piece_scale[PIECE_COUNT];

struct PieceGPU {
    GLuint vao = 0, vbo = 0;
    int num_vertices = 0;
};

struct BoardPiece {
    PieceType type;
    bool is_white;
    int col, row;
    bool alive = true;
};

struct CastlingRights {
    bool white_king_moved = false;
    bool black_king_moved = false;
    bool white_rook_a_moved = false;
    bool white_rook_h_moved = false;
    bool black_rook_a_moved = false;
    bool black_rook_h_moved = false;
};

struct BoardSnapshot {
    std::vector<BoardPiece> pieces;
    bool white_turn;
    CastlingRights castling;
    std::string last_move;
};

struct GameState {
    std::vector<BoardPiece> pieces;
    int grid[8][8];
    bool white_turn = true;
    CastlingRights castling;

    int selected_col = -1, selected_row = -1;
    std::vector<std::pair<int,int>> valid_moves;

    bool game_over = false;
    std::string game_result;

    bool ai_thinking = false;
    bool ai_animating = false;
    int64_t ai_anim_start = 0;
    float ai_anim_duration = 0.5f;
    int ai_from_col = 0, ai_from_row = 0, ai_to_col = 0, ai_to_row = 0;
    unsigned int ai_anim_tick = 0;

    bool analysis_mode = false;
    int analysis_index = 0;

    std::vector<std::string> move_history;
    std::vector<float> score_history;
    std::vector<BoardSnapshot> snapshots;

    int64_t anim_start_time = 0;
    unsigned int tick_id = 0;

    std::vector<BoardPiece> live_pieces;
    bool live_white_turn = true;
    CastlingRights live_castling;

    void rebuild_grid();
    void take_snapshot(const std::string& move_uci = "");
    void restore_snapshot(int index);
};

// Board constants
constexpr float SQ = 1.0f;
constexpr float BOARD_Y = 0.0f;
constexpr float BASE_PIECE_SCALE = 0.55f;

void square_center(int col, int row, float& x, float& z);
bool in_bounds(int c, int r);
