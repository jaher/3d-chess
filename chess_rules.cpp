#include "chess_rules.h"
#include "ai_player.h"
#include <cmath>
#include <cstdio>

// ---------------------------------------------------------------------------
// Position evaluation
// ---------------------------------------------------------------------------
static const float piece_value[PIECE_COUNT] = {
    0.0f, 9.0f, 3.25f, 3.0f, 5.0f, 1.0f
};

static const float center_bonus[8][8] = {
    {-0.2f, -0.1f, 0.0f, 0.0f, 0.0f, 0.0f, -0.1f, -0.2f},
    {-0.1f,  0.0f, 0.1f, 0.1f, 0.1f, 0.1f,  0.0f, -0.1f},
    { 0.0f,  0.1f, 0.2f, 0.25f,0.25f,0.2f,  0.1f,  0.0f},
    { 0.0f,  0.1f, 0.25f,0.3f, 0.3f, 0.25f, 0.1f,  0.0f},
    { 0.0f,  0.1f, 0.25f,0.3f, 0.3f, 0.25f, 0.1f,  0.0f},
    { 0.0f,  0.1f, 0.2f, 0.25f,0.25f,0.2f,  0.1f,  0.0f},
    {-0.1f,  0.0f, 0.1f, 0.1f, 0.1f, 0.1f,  0.0f, -0.1f},
    {-0.2f, -0.1f, 0.0f, 0.0f, 0.0f, 0.0f, -0.1f, -0.2f},
};

float evaluate_position(const GameState& gs) {
    float score = 0.0f;
    for (const auto& p : gs.pieces) {
        if (!p.alive) continue;
        float val = piece_value[p.type];
        if (p.type == PAWN || p.type == KNIGHT || p.type == BISHOP)
            val += center_bonus[p.row][p.col];
        if (p.type == PAWN) {
            float advance = p.is_white ? p.row - 1.0f : 6.0f - p.row;
            val += advance * 0.05f;
        }
        score += p.is_white ? val : -val;
    }
    return score;
}

// ---------------------------------------------------------------------------
// Attack detection
// ---------------------------------------------------------------------------
bool is_square_attacked(const GameState& gs, int c, int r, bool by_white) {
    // Pawn attacks
    int pawn_dir = by_white ? -1 : 1;
    for (int dc : {-1, 1}) {
        int pc = c + dc, pr = r + pawn_dir;
        if (in_bounds(pc, pr)) {
            int idx = gs.grid[pr][pc];
            if (idx >= 0 && gs.pieces[idx].is_white == by_white &&
                gs.pieces[idx].type == PAWN)
                return true;
        }
    }

    // Knight attacks
    for (auto [dc, dr] : std::vector<std::pair<int,int>>{{1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}}) {
        int nc = c + dc, nr = r + dr;
        if (in_bounds(nc, nr)) {
            int idx = gs.grid[nr][nc];
            if (idx >= 0 && gs.pieces[idx].is_white == by_white &&
                gs.pieces[idx].type == KNIGHT)
                return true;
        }
    }

    // King attacks
    for (int dc = -1; dc <= 1; dc++)
        for (int dr = -1; dr <= 1; dr++) {
            if (!dc && !dr) continue;
            int nc = c + dc, nr = r + dr;
            if (in_bounds(nc, nr)) {
                int idx = gs.grid[nr][nc];
                if (idx >= 0 && gs.pieces[idx].is_white == by_white &&
                    gs.pieces[idx].type == KING)
                    return true;
            }
        }

    // Sliding: rook/queen
    for (auto [dc, dr] : std::vector<std::pair<int,int>>{{1,0},{-1,0},{0,1},{0,-1}}) {
        int sc = c + dc, sr = r + dr;
        while (in_bounds(sc, sr)) {
            int idx = gs.grid[sr][sc];
            if (idx >= 0) {
                if (gs.pieces[idx].is_white == by_white &&
                    (gs.pieces[idx].type == ROOK || gs.pieces[idx].type == QUEEN))
                    return true;
                break;
            }
            sc += dc; sr += dr;
        }
    }

    // Sliding: bishop/queen
    for (auto [dc, dr] : std::vector<std::pair<int,int>>{{1,1},{1,-1},{-1,1},{-1,-1}}) {
        int sc = c + dc, sr = r + dr;
        while (in_bounds(sc, sr)) {
            int idx = gs.grid[sr][sc];
            if (idx >= 0) {
                if (gs.pieces[idx].is_white == by_white &&
                    (gs.pieces[idx].type == BISHOP || gs.pieces[idx].type == QUEEN))
                    return true;
                break;
            }
            sc += dc; sr += dr;
        }
    }

    return false;
}

bool is_in_check(const GameState& gs, bool white_king) {
    for (const auto& p : gs.pieces) {
        if (p.alive && p.type == KING && p.is_white == white_king)
            return is_square_attacked(gs, p.col, p.row, !white_king);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Move generation
// ---------------------------------------------------------------------------
static void add_slide_moves(const GameState& gs, int col, int row, bool is_white,
                            int dc, int dr, std::vector<std::pair<int,int>>& moves) {
    int c = col + dc, r = row + dr;
    while (in_bounds(c, r)) {
        int idx = gs.grid[r][c];
        if (idx == -1) {
            moves.push_back({c, r});
        } else {
            if (gs.pieces[idx].is_white != is_white)
                moves.push_back({c, r});
            break;
        }
        c += dc; r += dr;
    }
}

std::vector<std::pair<int,int>> generate_moves(const GameState& gs, int col, int row) {
    std::vector<std::pair<int,int>> moves;
    int idx = gs.grid[row][col];
    if (idx < 0) return moves;

    const auto& piece = gs.pieces[idx];
    bool w = piece.is_white;

    auto try_step = [&](int c, int r) {
        if (!in_bounds(c, r)) return;
        int ti = gs.grid[r][c];
        if (ti == -1 || gs.pieces[ti].is_white != w)
            moves.push_back({c, r});
    };

    switch (piece.type) {
    case PAWN: {
        int dir = w ? 1 : -1;
        int start_row = w ? 1 : 6;
        if (in_bounds(col, row + dir) && gs.grid[row + dir][col] == -1) {
            moves.push_back({col, row + dir});
            if (row == start_row && gs.grid[row + 2*dir][col] == -1)
                moves.push_back({col, row + 2*dir});
        }
        for (int dc : {-1, 1}) {
            int nc = col + dc, nr = row + dir;
            if (in_bounds(nc, nr)) {
                int ti = gs.grid[nr][nc];
                if (ti >= 0 && gs.pieces[ti].is_white != w) {
                    moves.push_back({nc, nr});
                } else if (ti == -1 &&
                           nc == gs.ep_target_col &&
                           nr == gs.ep_target_row) {
                    // En passant: empty diagonal square that matches
                    // the ep target (set when the enemy pawn just
                    // double-pushed past us last ply).
                    moves.push_back({nc, nr});
                }
            }
        }
        break;
    }
    case ROOK:
        for (auto [dc, dr] : std::vector<std::pair<int,int>>{{1,0},{-1,0},{0,1},{0,-1}})
            add_slide_moves(gs, col, row, w, dc, dr, moves);
        break;
    case BISHOP:
        for (auto [dc, dr] : std::vector<std::pair<int,int>>{{1,1},{1,-1},{-1,1},{-1,-1}})
            add_slide_moves(gs, col, row, w, dc, dr, moves);
        break;
    case QUEEN:
        for (auto [dc, dr] : std::vector<std::pair<int,int>>{{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}})
            add_slide_moves(gs, col, row, w, dc, dr, moves);
        break;
    case KNIGHT:
        for (auto [dc, dr] : std::vector<std::pair<int,int>>{{1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}})
            try_step(col + dc, row + dr);
        break;
    case KING: {
        for (int dc = -1; dc <= 1; dc++)
            for (int dr = -1; dr <= 1; dr++)
                if (dc || dr) try_step(col + dc, row + dr);

        bool king_moved = w ? gs.castling.white_king_moved : gs.castling.black_king_moved;
        int home_row = w ? 0 : 7;

        // King starts at col 3 (e-file). Kingside = toward col 0 (h-file), queenside = toward col 7 (a-file)
        if (!king_moved && col == 3 && row == home_row) {
            // Kingside: king to col 1 (g-file), rook h (col 0) to col 2 (f-file)
            bool rook_h = w ? gs.castling.white_rook_h_moved : gs.castling.black_rook_h_moved;
            if (!rook_h &&
                gs.grid[home_row][2] == -1 && gs.grid[home_row][1] == -1 &&
                !is_square_attacked(gs, 3, home_row, !w) &&
                !is_square_attacked(gs, 2, home_row, !w) &&
                !is_square_attacked(gs, 1, home_row, !w)) {
                moves.push_back({1, home_row});
            }
            // Queenside: king to col 5 (c-file), rook a (col 7) to col 4 (d-file)
            bool rook_a = w ? gs.castling.white_rook_a_moved : gs.castling.black_rook_a_moved;
            if (!rook_a &&
                gs.grid[home_row][4] == -1 && gs.grid[home_row][5] == -1 &&
                gs.grid[home_row][6] == -1 &&
                !is_square_attacked(gs, 3, home_row, !w) &&
                !is_square_attacked(gs, 4, home_row, !w) &&
                !is_square_attacked(gs, 5, home_row, !w)) {
                moves.push_back({5, home_row});
            }
        }
        break;
    }
    default: break;
    }
    return moves;
}

// ---------------------------------------------------------------------------
// Legal move filtering
// ---------------------------------------------------------------------------
bool move_leaves_in_check(GameState& gs, int from_c, int from_r, int to_c, int to_r, bool is_white) {
    int src_idx = gs.grid[from_r][from_c];
    int dst_idx = gs.grid[to_r][to_c];

    int old_col = gs.pieces[src_idx].col;
    int old_row = gs.pieces[src_idx].row;
    bool dst_was_alive = false;

    // En passant: a pawn moving diagonally to an empty square captures
    // the enemy pawn that lives at (to_c, from_r), not on the
    // destination. Without this, the "en passant pin" case (capture
    // exposes own king along the now-cleared rank) can't be detected.
    int ep_pawn_idx = -1;
    bool ep_pawn_was_alive = false;
    bool is_ep = false;
    if (gs.pieces[src_idx].type == PAWN && std::abs(from_c - to_c) == 1 &&
        dst_idx == -1) {
        int candidate = gs.grid[from_r][to_c];
        if (candidate >= 0 &&
            gs.pieces[candidate].type == PAWN &&
            gs.pieces[candidate].is_white != gs.pieces[src_idx].is_white) {
            is_ep = true;
            ep_pawn_idx = candidate;
            ep_pawn_was_alive = gs.pieces[ep_pawn_idx].alive;
            gs.pieces[ep_pawn_idx].alive = false;
            gs.grid[from_r][to_c] = -1;
        }
    }

    gs.pieces[src_idx].col = to_c;
    gs.pieces[src_idx].row = to_r;
    gs.grid[from_r][from_c] = -1;
    gs.grid[to_r][to_c] = src_idx;

    if (dst_idx >= 0) {
        dst_was_alive = gs.pieces[dst_idx].alive;
        gs.pieces[dst_idx].alive = false;
    }

    bool in_check = is_in_check(gs, is_white);

    gs.pieces[src_idx].col = old_col;
    gs.pieces[src_idx].row = old_row;
    gs.grid[from_r][from_c] = src_idx;
    gs.grid[to_r][to_c] = dst_idx;

    if (dst_idx >= 0)
        gs.pieces[dst_idx].alive = dst_was_alive;

    if (is_ep) {
        gs.pieces[ep_pawn_idx].alive = ep_pawn_was_alive;
        gs.grid[from_r][to_c] = ep_pawn_idx;
    }

    return in_check;
}

std::vector<std::pair<int,int>> generate_legal_moves(GameState& gs, int col, int row) {
    int idx = gs.grid[row][col];
    if (idx < 0) return {};

    bool is_white = gs.pieces[idx].is_white;
    auto pseudo = generate_moves(gs, col, row);

    std::vector<std::pair<int,int>> legal;
    for (const auto& [tc, tr] : pseudo) {
        if (!move_leaves_in_check(gs, col, row, tc, tr, is_white))
            legal.push_back({tc, tr});
    }
    return legal;
}

bool has_any_legal_move(GameState& gs, bool is_white) {
    for (const auto& p : gs.pieces) {
        if (!p.alive || p.is_white != is_white) continue;
        auto moves = generate_legal_moves(gs, p.col, p.row);
        if (!moves.empty()) return true;
    }
    return false;
}

void check_game_over(GameState& gs) {
    bool current_in_check = is_in_check(gs, gs.white_turn);
    bool has_moves = has_any_legal_move(gs, gs.white_turn);

    if (!has_moves) {
        gs.game_over = true;
        if (current_in_check) {
            gs.game_result = gs.white_turn ? "Black wins by checkmate!"
                                           : "White wins by checkmate!";
        } else {
            gs.game_result = "Draw by stalemate!";
        }
        std::printf("%s\n", gs.game_result.c_str());
    }
}

// ---------------------------------------------------------------------------
// Move execution
// ---------------------------------------------------------------------------
void execute_move(GameState& gs, int from_col, int from_row, int to_col, int to_row) {
    int src_idx = gs.grid[from_row][from_col];
    int dst_idx = gs.grid[to_row][to_col];
    bool is_white = gs.pieces[src_idx].is_white;
    PieceType src_type = gs.pieces[src_idx].type;

    // EP target is valid for one ply. Snapshot the previous value (so
    // we can recognise an ep capture) then clear; new value is set
    // below if this move is itself a pawn double-push.
    int prev_ep_col = gs.ep_target_col;
    int prev_ep_row = gs.ep_target_row;
    gs.ep_target_col = -1;
    gs.ep_target_row = -1;

    // King captured
    if (dst_idx >= 0 && gs.pieces[dst_idx].type == KING) {
        gs.pieces[dst_idx].alive = false;
        gs.pieces[src_idx].col = to_col;
        gs.pieces[src_idx].row = to_row;
        std::string uci = move_to_uci(from_col, from_row, to_col, to_row);
        gs.move_history.push_back(uci);
        gs.rebuild_grid();
        gs.white_turn = !gs.white_turn;
        gs.score_history.push_back(evaluate_position(gs));
        gs.take_snapshot(uci);
        gs.game_over = true;
        gs.game_result = is_white ? "White wins!" : "Black wins!";
        std::printf("%s (king captured)\n", gs.game_result.c_str());
        return;
    }

    // En passant capture: pawn moves diagonally onto the previous-ply
    // ep target (an empty square). The actual captured pawn lives one
    // rank back from the destination, on the moving pawn's file of
    // arrival. Mark it dead before the standard "kill destination"
    // branch (which would no-op here since dst_idx == -1).
    if (src_type == PAWN && dst_idx == -1 &&
        std::abs(to_col - from_col) == 1 &&
        to_col == prev_ep_col && to_row == prev_ep_row) {
        int bypassed = gs.grid[from_row][to_col];
        if (bypassed >= 0) gs.pieces[bypassed].alive = false;
    }

    if (dst_idx >= 0)
        gs.pieces[dst_idx].alive = false;

    // Castling
    if (gs.pieces[src_idx].type == KING) {
        if (is_white) gs.castling.white_king_moved = true;
        else gs.castling.black_king_moved = true;

        if (std::abs(to_col - from_col) == 2) {
            // Kingside: king to col 1, rook from col 0 to col 2
            // Queenside: king to col 5, rook from col 7 to col 4
            int rook_from = (to_col == 1) ? 0 : 7;
            int rook_to = (to_col == 1) ? 2 : 4;
            int rook_idx = gs.grid[from_row][rook_from];
            if (rook_idx >= 0) {
                gs.pieces[rook_idx].col = rook_to;
                gs.pieces[rook_idx].row = from_row;
            }
        }
    }
    // h-rook at col 0, a-rook at col 7 (reversed board)
    if (gs.pieces[src_idx].type == ROOK) {
        if (is_white) {
            if (from_col == 7 && from_row == 0) gs.castling.white_rook_a_moved = true;
            if (from_col == 0 && from_row == 0) gs.castling.white_rook_h_moved = true;
        } else {
            if (from_col == 7 && from_row == 7) gs.castling.black_rook_a_moved = true;
            if (from_col == 0 && from_row == 7) gs.castling.black_rook_h_moved = true;
        }
    }
    // Lose castling if rook captured on its home square
    if (to_col == 7 && to_row == 0) gs.castling.white_rook_a_moved = true;
    if (to_col == 0 && to_row == 0) gs.castling.white_rook_h_moved = true;
    if (to_col == 7 && to_row == 7) gs.castling.black_rook_a_moved = true;
    if (to_col == 0 && to_row == 7) gs.castling.black_rook_h_moved = true;

    gs.pieces[src_idx].col = to_col;
    gs.pieces[src_idx].row = to_row;

    // Pawn promotion
    if (gs.pieces[src_idx].type == PAWN) {
        if ((is_white && to_row == 7) || (!is_white && to_row == 0))
            gs.pieces[src_idx].type = QUEEN;
    }

    // Pawn double-push sets the new ep target on the square the pawn
    // skipped over. Anything else leaves the cleared ep target alone.
    if (src_type == PAWN && std::abs(to_row - from_row) == 2) {
        gs.ep_target_col = from_col;
        gs.ep_target_row = (from_row + to_row) / 2;
    }

    std::string uci = move_to_uci(from_col, from_row, to_col, to_row);
    gs.move_history.push_back(uci);
    gs.rebuild_grid();
    gs.white_turn = !gs.white_turn;
    gs.score_history.push_back(evaluate_position(gs));
    gs.take_snapshot(uci);
    check_game_over(gs);
}

// ---------------------------------------------------------------------------
// Starting position
// ---------------------------------------------------------------------------
std::vector<BoardPiece> build_starting_position() {
    std::vector<BoardPiece> pieces;
    // Reversed so col 7 = a-file (screen-left with rot_y=180), col 0 = h-file
    PieceType back_rank[8] = {ROOK, KNIGHT, BISHOP, KING, QUEEN, BISHOP, KNIGHT, ROOK};
    for (int c = 0; c < 8; c++) {
        pieces.push_back({back_rank[c], true, c, 0, true});
        pieces.push_back({PAWN, true, c, 1, true});
    }
    for (int c = 0; c < 8; c++) {
        pieces.push_back({PAWN, false, c, 6, true});
        pieces.push_back({back_rank[c], false, c, 7, true});
    }
    return pieces;
}

// ---------------------------------------------------------------------------
// UCI to algebraic notation
// ---------------------------------------------------------------------------
std::string uci_to_algebraic(const BoardSnapshot& before, const std::string& uci) {
    if (uci.size() < 4) return uci;

    // UCI uses standard files (a=0..h=7), convert to internal cols (a=7..h=0)
    int fc_internal = 7 - (uci[0] - 'a');
    int fr = uci[1] - '1';
    int tc_internal = 7 - (uci[2] - 'a');
    int tr = uci[3] - '1';

    // Standard file indices for display
    int fc_file = uci[0] - 'a'; // 0=a, 7=h
    int tc_file = uci[2] - 'a';

    if (!in_bounds(fc_internal, fr) || !in_bounds(tc_internal, tr)) return uci;

    // Find piece at source (using internal col)
    const BoardPiece* src = nullptr;
    for (const auto& p : before.pieces) {
        if (p.alive && p.col == fc_internal && p.row == fr) { src = &p; break; }
    }
    if (!src) return uci;

    // Castling
    if (src->type == KING && std::abs(tc_internal - fc_internal) == 2) {
        // Kingside = toward h-file (col 0), queenside = toward a-file (col 7)
        return (tc_internal < fc_internal) ? "O-O" : "O-O-O";
    }

    // Is it a capture? (use internal cols for piece lookup)
    bool capture = false;
    for (const auto& p : before.pieces) {
        if (p.alive && p.col == tc_internal && p.row == tr && p.is_white != src->is_white) {
            capture = true; break;
        }
    }
    // En passant: pawn moving diagonally onto the snapshot's ep target
    // (destination is empty, so the loop above missed it). Render with
    // an 'x' just like any other pawn capture.
    if (!capture && src->type == PAWN && fc_internal != tc_internal &&
        tc_internal == before.ep_target_col && tr == before.ep_target_row) {
        capture = true;
    }

    std::string result;
    char file_from = 'a' + fc_file;
    char file_to = 'a' + tc_file;
    char rank_to = '1' + tr;

    const char* piece_chars = "KQBNR";

    if (src->type == PAWN) {
        if (capture) {
            result += file_from;
            result += 'x';
        }
        result += file_to;
        result += rank_to;
        // Promotion
        if (uci.size() >= 5) {
            result += '=';
            result += static_cast<char>(std::toupper(uci[4]));
        } else if ((src->is_white && tr == 7) || (!src->is_white && tr == 0)) {
            result += "=Q";
        }
    } else {
        result += piece_chars[src->type]; // K=0,Q=1,B=2,N=3,R=4

        // Disambiguation: check if another piece of same type can reach same square
        bool need_file = false, need_rank = false;
        for (const auto& p : before.pieces) {
            if (!p.alive || &p == src) continue;
            if (p.type != src->type || p.is_white != src->is_white) continue;
            if (p.col == tc_internal && p.row == tr) continue;
            bool can_reach = false;
            if (p.type == KNIGHT) {
                int dx = std::abs(p.col - tc_internal), dy = std::abs(p.row - tr);
                can_reach = (dx == 1 && dy == 2) || (dx == 2 && dy == 1);
            } else if (p.type == ROOK || p.type == QUEEN) {
                can_reach = (p.col == tc_internal || p.row == tr);
            }
            if (p.type == BISHOP || p.type == QUEEN) {
                if (std::abs(p.col - tc_internal) == std::abs(p.row - tr))
                    can_reach = true;
            }
            if (can_reach) {
                if (p.col != fc_internal) need_file = true;
                else need_rank = true;
            }
        }
        if (need_file) result += file_from;
        if (need_rank) result += static_cast<char>('1' + fr);

        if (capture) result += 'x';
        result += file_to;
        result += rank_to;
    }

    // Check / checkmate suffix — simulate the move on a temp state
    {
        GameState tmp;
        tmp.pieces = before.pieces;
        tmp.white_turn = before.white_turn;
        tmp.castling = before.castling;
        tmp.rebuild_grid();
        execute_move(tmp, fc_internal, fr, tc_internal, tr);
        // After execute_move, tmp.white_turn is the OPPONENT (side to move next)
        bool opp_in_check = is_in_check(tmp, tmp.white_turn);
        if (opp_in_check) {
            bool has_moves = has_any_legal_move(tmp, tmp.white_turn);
            if (!has_moves) result += '#';
            else result += '+';
        }
    }

    return result;
}
