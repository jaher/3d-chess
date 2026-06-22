#include "move_reason.h"

#include "chess_rules.h"   // is_square_attacked

namespace {

const char* piece_name(PieceType t) {
    switch (t) {
        case QUEEN:  return "queen";
        case ROOK:   return "rook";
        case BISHOP: return "bishop";
        case KNIGHT: return "knight";
        case PAWN:   return "pawn";
        default:     return "king";
    }
}

// Algebraic square name from a piece's internal col/row. The internal
// column is the file mirrored (file_to_internal_col = 7 - file), so map
// it back to a file letter; the row maps straight to the rank (row 0 =
// rank 1).
std::string square_name(int col, int row) {
    std::string s;
    s += static_cast<char>('a' + (7 - col));
    s += static_cast<char>('1' + row);
    return s;
}

}  // namespace

std::string generate_why_reason(const GameState& gs, MoveClass cls) {
    const bool mover_white = !gs.white_turn;
    static const float val[PIECE_COUNT] = {
        0.0f, 9.0f, 3.25f, 3.0f, 5.0f, 1.0f
    };
    // The most valuable friendly piece the opponent attacks that nothing
    // defends — a hung piece. (Doesn't model defenders' values, but a
    // free piece is the common, well-reading case.)
    int hang_c = -1, hang_r = -1; PieceType hang_t = PAWN; float hang_v = 0.0f;
    for (const auto& p : gs.pieces) {
        if (!p.alive || p.is_white != mover_white || p.type == KING) continue;
        bool attacked = is_square_attacked(gs, p.col, p.row, !mover_white);
        bool defended = is_square_attacked(gs, p.col, p.row,  mover_white);
        if (attacked && !defended && val[p.type] > hang_v) {
            hang_v = val[p.type]; hang_c = p.col; hang_r = p.row; hang_t = p.type;
        }
    }
    if (hang_c >= 0) {
        return std::string("Leaves the ") + piece_name(hang_t) +
               " on " + square_name(hang_c, hang_r) + " hanging.";
    }
    switch (cls) {
        case MoveClass::Blunder:    return "A losing move - a much stronger option was available.";
        case MoveClass::MissedWin:  return "Lets a winning advantage slip away.";
        case MoveClass::Mistake:    return "Inaccurate - concedes a clear edge.";
        case MoveClass::Miss:       return "Misses a clearly stronger move.";
        case MoveClass::Inaccuracy: return "A small concession; a more precise move held more.";
        default:                    return "";
    }
}
