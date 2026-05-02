#pragma once

#include <cstdint>
#include <string>
#include <vector>

// NOTE: this header must not depend on any OpenGL header, so that the
// pure-logic layer (chess rules, FEN parsing, etc.) can be compiled
// into the test binary without a GL sysroot on the include path. The
// PieceGPU struct, which used to live here, has moved to
// board_renderer.h where the GLuint type is actually needed.

enum PieceType { KING, QUEEN, BISHOP, KNIGHT, ROOK, PAWN, PIECE_COUNT };
enum GameMode {
    MODE_MENU,
    MODE_PREGAME,          // pre-game setup screen (side + ELO)
    MODE_PLAYING,
    MODE_CHALLENGE_SELECT,
    MODE_CHALLENGE,
    MODE_OPTIONS,          // options panel accessed from the main menu
};

extern const char* piece_filenames[PIECE_COUNT];
extern const float piece_scale[PIECE_COUNT];

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
    // En passant target square (the square BEHIND the pawn that just
    // double-pushed, where an enemy pawn could land to capture). -1
    // means no ep target this ply. Mirrored on GameState.
    int ep_target_col = -1, ep_target_row = -1;
};

struct GameState {
    std::vector<BoardPiece> pieces;
    int grid[8][8];
    bool white_turn = true;
    CastlingRights castling;
    // En passant target valid for the ply that immediately follows a
    // pawn double-push. Cleared by every other move. See FEN field 4.
    int ep_target_col = -1, ep_target_row = -1;

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
    // The animation pipeline (ai_animating + tick_ai_animation) is
    // also reused for sensor-driven moves in two-player mode so the
    // user sees the same arrow + piece-flying visual when their
    // opponent plays on the physical board. In that case the piece
    // is *already* at its destination on the board, so the post-
    // animation app_chessnut_sync_board would needlessly re-drive
    // the firmware. This flag is set by the sensor handler to
    // suppress that final sync; tick_ai_animation clears it.
    bool ai_anim_skip_chessnut_sync = false;

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
