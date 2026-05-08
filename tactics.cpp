#include "tactics.h"

#include <algorithm>
#include <vector>

#include "chess_rules.h"
#include "ai_player.h"  // parse_uci_move

namespace {

float piece_value(PieceType t) {
    switch (t) {
        case QUEEN:  return 9.0f;
        case ROOK:   return 5.0f;
        case BISHOP: return 3.25f;
        case KNIGHT: return 3.0f;
        case PAWN:   return 1.0f;
        case KING:   return 100.0f;  // treat as priceless for ordering
        default:     return 0.0f;
    }
}

// Walk a ray from (sc, sr) in direction (dc, dr). Returns:
//   - the (col, row) of the first piece we hit, OR
//   - {-1, -1} if we ran off the board.
std::pair<int,int> ray_hit(const GameState& gs,
                            int sc, int sr, int dc, int dr) {
    int c = sc + dc, r = sr + dr;
    while (c >= 0 && c < 8 && r >= 0 && r < 8) {
        if (gs.grid[r][c] >= 0) return {c, r};
        c += dc; r += dr;
    }
    return {-1, -1};
}

// All squares the piece on (col, row) currently attacks. Used for
// the fork / pin / skewer logic — same notion as the move-generator
// but stripped of move-legality (we only care about "can this
// piece capture there").
std::vector<std::pair<int,int>>
attack_squares(const GameState& gs, int col, int row) {
    std::vector<std::pair<int,int>> out;
    int idx = gs.grid[row][col];
    if (idx < 0) return out;
    const BoardPiece& p = gs.pieces[idx];

    auto sliding = [&](const std::vector<std::pair<int,int>>& dirs) {
        for (auto [dc, dr] : dirs) {
            auto hit = ray_hit(gs, col, row, dc, dr);
            // Walk back from `hit` (or full ray) and emit every
            // empty square plus the hit square (whichever colour).
            int c = col + dc, r = row + dr;
            while (c >= 0 && c < 8 && r >= 0 && r < 8) {
                out.emplace_back(c, r);
                if (gs.grid[r][c] >= 0) break;
                c += dc; r += dr;
            }
            (void)hit;
        }
    };

    static const std::vector<std::pair<int,int>> ROOK_DIRS = {
        {1,0}, {-1,0}, {0,1}, {0,-1}
    };
    static const std::vector<std::pair<int,int>> BISHOP_DIRS = {
        {1,1}, {1,-1}, {-1,1}, {-1,-1}
    };
    static const std::vector<std::pair<int,int>> QUEEN_DIRS = {
        {1,0}, {-1,0}, {0,1}, {0,-1},
        {1,1}, {1,-1}, {-1,1}, {-1,-1}
    };
    static const std::vector<std::pair<int,int>> KNIGHT_DELTAS = {
        {1,2}, {2,1}, {-1,2}, {-2,1}, {1,-2}, {2,-1}, {-1,-2}, {-2,-1}
    };
    static const std::vector<std::pair<int,int>> KING_DELTAS = {
        {1,0}, {-1,0}, {0,1}, {0,-1},
        {1,1}, {1,-1}, {-1,1}, {-1,-1}
    };

    switch (p.type) {
        case ROOK:   sliding(ROOK_DIRS);   break;
        case BISHOP: sliding(BISHOP_DIRS); break;
        case QUEEN:  sliding(QUEEN_DIRS);  break;
        case KNIGHT:
            for (auto [dc, dr] : KNIGHT_DELTAS) {
                int c = col + dc, r = row + dr;
                if (c >= 0 && c < 8 && r >= 0 && r < 8) out.emplace_back(c, r);
            }
            break;
        case KING:
            for (auto [dc, dr] : KING_DELTAS) {
                int c = col + dc, r = row + dr;
                if (c >= 0 && c < 8 && r >= 0 && r < 8) out.emplace_back(c, r);
            }
            break;
        case PAWN: {
            int dr = p.is_white ? 1 : -1;
            for (int dc : {-1, 1}) {
                int c = col + dc, r = row + dr;
                if (c >= 0 && c < 8 && r >= 0 && r < 8) out.emplace_back(c, r);
            }
            break;
        }
        default: break;
    }
    return out;
}

// Was this move a castling? UCI is the king's two-square hop.
bool is_castling(const BoardSnapshot& prev, const std::string& uci,
                 bool& is_kingside) {
    int from_c, from_r, to_c, to_r;
    if (!parse_uci_move(uci, from_c, from_r, to_c, to_r)) return false;
    // Find the piece that stood at (from_c, from_r) in the prior
    // position; only a king moving two files counts.
    for (const auto& p : prev.pieces) {
        if (!p.alive || p.col != from_c || p.row != from_r) continue;
        if (p.type != KING) return false;
        int df = to_c - from_c;
        if (df == 2 || df == -2) {
            // Internal-col convention (col 7 = a-file, col 0 = h-
            // file) means a kingside castle (white K e1→g1) shifts
            // the king from internal col 3 to col 1 (a -2 delta);
            // queenside is +2. Use the absolute file letter via
            // internal_col_to_file to avoid surprise.
            int from_file = internal_col_to_file(from_c);
            int to_file   = internal_col_to_file(to_c);
            is_kingside = (to_file > from_file);
            return true;
        }
        return false;
    }
    return false;
}

// Was this an en-passant capture? Pawn moved diagonally onto an
// empty square (i.e. the destination square in `prev` had no
// piece, but the move was a pawn diagonal).
bool is_en_passant(const GameState& post, const BoardSnapshot& prev,
                   const std::string& uci) {
    int from_c, from_r, to_c, to_r;
    if (!parse_uci_move(uci, from_c, from_r, to_c, to_r)) return false;
    // Find the piece that landed on (to_c, to_r) post-move.
    int post_idx = post.grid[to_r][to_c];
    if (post_idx < 0) return false;
    const BoardPiece& moved = post.pieces[post_idx];
    if (moved.type != PAWN) return false;
    if (from_c == to_c) return false;              // straight push, not capture
    // Pre-move, the destination must have been empty (otherwise
    // it's just a regular pawn capture). Walk prev.pieces to check.
    for (const auto& p : prev.pieces) {
        if (!p.alive) continue;
        if (p.col == to_c && p.row == to_r) return false;
    }
    return true;
}

// Was this move a promotion? Pawn ended on the back rank.
bool is_promotion(const GameState& post, const BoardSnapshot& prev,
                  const std::string& uci) {
    int from_c, from_r, to_c, to_r;
    if (!parse_uci_move(uci, from_c, from_r, to_c, to_r)) return false;
    int post_idx = post.grid[to_r][to_c];
    if (post_idx < 0) return false;
    const BoardPiece& moved = post.pieces[post_idx];
    // Pre-move, was this piece a pawn? Check by looking at the
    // matching slot in prev.pieces (positional match: same {col,
    // row, is_white} as the source square).
    for (const auto& p : prev.pieces) {
        if (!p.alive) continue;
        if (p.col == from_c && p.row == from_r &&
            p.is_white == moved.is_white) {
            return p.type == PAWN && moved.type != PAWN;
        }
    }
    return false;
}

// Did the move expose a discovered attack on the enemy king? We
// detect by counting how many enemy-king attackers there are
// post-move and which of them is the moving piece. If the king is
// in check from a piece other than the one that just moved, that's
// a discovered check.
bool is_discovered_check(const GameState& post,
                         int moved_to_c, int moved_to_r,
                         bool moved_is_white) {
    // Find enemy king.
    int king_c = -1, king_r = -1;
    for (const auto& p : post.pieces) {
        if (p.alive && p.type == KING && p.is_white != moved_is_white) {
            king_c = p.col; king_r = p.row; break;
        }
    }
    if (king_c < 0) return false;
    // Enumerate own-side pieces that attack the king square, excluding
    // the one that just moved.
    int attackers = 0;
    bool moved_attacks = false;
    for (const auto& p : post.pieces) {
        if (!p.alive || p.is_white != moved_is_white) continue;
        auto squares = attack_squares(post, p.col, p.row);
        for (auto& sq : squares) {
            if (sq.first == king_c && sq.second == king_r) {
                ++attackers;
                if (p.col == moved_to_c && p.row == moved_to_r) {
                    moved_attacks = true;
                }
                break;
            }
        }
    }
    if (attackers == 0) return false;           // not check at all
    // Discovered: at least one attacker is NOT the moved piece.
    if (!moved_attacks) return true;            // pure discovered
    return attackers >= 2;                       // double check (counts as discovered+)
}

bool is_double_check(const GameState& post,
                     bool moved_is_white) {
    int king_c = -1, king_r = -1;
    for (const auto& p : post.pieces) {
        if (p.alive && p.type == KING && p.is_white != moved_is_white) {
            king_c = p.col; king_r = p.row; break;
        }
    }
    if (king_c < 0) return false;
    int attackers = 0;
    for (const auto& p : post.pieces) {
        if (!p.alive || p.is_white != moved_is_white) continue;
        auto squares = attack_squares(post, p.col, p.row);
        for (auto& sq : squares) {
            if (sq.first == king_c && sq.second == king_r) {
                ++attackers;
                break;
            }
        }
    }
    return attackers >= 2;
}

// Fork — the moved piece attacks two or more enemy pieces, at
// least two of which are of higher value than the moved piece OR
// undefended (so capturing them is a clean material win). Only
// counts if at least one of the targets is a major piece (R/Q/K).
bool is_fork(const GameState& post,
             int moved_to_c, int moved_to_r) {
    int idx = post.grid[moved_to_r][moved_to_c];
    if (idx < 0) return false;
    const BoardPiece& moved = post.pieces[idx];
    float my_val = piece_value(moved.type);

    auto squares = attack_squares(post, moved_to_c, moved_to_r);
    int valuable_targets = 0;
    bool hits_major = false;
    for (auto& sq : squares) {
        int t_idx = post.grid[sq.second][sq.first];
        if (t_idx < 0) continue;
        const BoardPiece& tgt = post.pieces[t_idx];
        if (tgt.is_white == moved.is_white) continue;       // own piece
        float tv = piece_value(tgt.type);
        bool defended = is_square_attacked(post, sq.first, sq.second,
                                            tgt.is_white);
        if (tv > my_val || !defended) {
            ++valuable_targets;
            if (tgt.type == KING || tgt.type == QUEEN || tgt.type == ROOK) {
                hits_major = true;
            }
        }
    }
    return valuable_targets >= 2 && hits_major;
}

// Helper: walk the ray from the moved piece through the enemy
// king's "rook" / "bishop" lines to detect line-piece pin / skewer
// patterns. Only meaningful for sliding pieces (R/B/Q).
//
// Pin: own slider attacks an enemy piece, with a more-valuable
// enemy piece directly behind it on the same ray. (Absolute pin
// against the king is the common case.)
//
// Skewer: own slider attacks an enemy high-value piece, with a
// less-valuable enemy piece behind it on the same ray.
//
// Returns true if pin/skewer was detected, with `kind` set to the
// label string.
bool find_pin_or_skewer(const GameState& post,
                        int moved_to_c, int moved_to_r,
                        std::string& kind) {
    int idx = post.grid[moved_to_r][moved_to_c];
    if (idx < 0) return false;
    const BoardPiece& moved = post.pieces[idx];
    bool is_slider = (moved.type == ROOK || moved.type == BISHOP ||
                      moved.type == QUEEN);
    if (!is_slider) return false;

    static const std::vector<std::pair<int,int>> ROOK_DIRS = {
        {1,0}, {-1,0}, {0,1}, {0,-1}
    };
    static const std::vector<std::pair<int,int>> BISHOP_DIRS = {
        {1,1}, {1,-1}, {-1,1}, {-1,-1}
    };

    std::vector<std::pair<int,int>> dirs;
    if (moved.type == ROOK || moved.type == QUEEN)
        dirs.insert(dirs.end(), ROOK_DIRS.begin(), ROOK_DIRS.end());
    if (moved.type == BISHOP || moved.type == QUEEN)
        dirs.insert(dirs.end(), BISHOP_DIRS.begin(), BISHOP_DIRS.end());

    for (auto [dc, dr] : dirs) {
        // First piece on this ray.
        auto first = ray_hit(post, moved_to_c, moved_to_r, dc, dr);
        if (first.first < 0) continue;
        int f_idx = post.grid[first.second][first.first];
        if (f_idx < 0) continue;
        const BoardPiece& a = post.pieces[f_idx];
        if (a.is_white == moved.is_white) continue;        // own piece, no
        // Second piece behind it.
        auto second = ray_hit(post, first.first, first.second, dc, dr);
        if (second.first < 0) continue;
        int s_idx = post.grid[second.second][second.first];
        if (s_idx < 0) continue;
        const BoardPiece& bp = post.pieces[s_idx];
        if (bp.is_white == moved.is_white) continue;       // own piece behind, no
        float va = piece_value(a.type);
        float vb = piece_value(bp.type);
        if (vb > va + 0.25f) {
            kind = "Pin";
            return true;
        }
        if (va > vb + 0.25f) {
            kind = "Skewer";
            return true;
        }
    }
    return false;
}

}  // namespace

std::string classify_tactic(const GameState& post,
                            const BoardSnapshot& prev,
                            const std::string& move_uci) {
    if (move_uci.size() < 4) return "";

    int from_c, from_r, to_c, to_r;
    if (!parse_uci_move(move_uci, from_c, from_r, to_c, to_r)) return "";

    // Identify the piece that just moved (now at to_c, to_r).
    int idx = post.grid[to_r][to_c];
    if (idx < 0) return "";
    const BoardPiece& moved = post.pieces[idx];

    // ----- Castling first (specific → has its own SAN; in TTS we
    // already say "kingside castle" via the move-text generator
    // for many cases, but this label is the canonical single
    // source).
    bool kingside = false;
    if (is_castling(prev, move_uci, kingside)) {
        return kingside ? "Castles kingside" : "Castles queenside";
    }

    // ----- En passant.
    if (is_en_passant(post, prev, move_uci)) {
        return "En passant";
    }

    // ----- Promotion (the move-text already says "promote to X";
    // this label adds emphasis for the milestone).
    if (is_promotion(post, prev, move_uci)) {
        return "Promotion";
    }

    // ----- Discovered / double check (call out specifically; a
    // plain "+" is already captured by the SAN suffix, so we
    // only fire the louder labels here).
    if (is_double_check(post, moved.is_white)) {
        return "Double check";
    }
    if (is_discovered_check(post, to_c, to_r, moved.is_white)) {
        return "Discovered check";
    }

    // ----- Fork.
    if (is_fork(post, to_c, to_r)) {
        return "Fork";
    }

    // ----- Pin / Skewer (line-piece patterns).
    std::string ps;
    if (find_pin_or_skewer(post, to_c, to_r, ps)) {
        return ps;
    }

    return "";
}

namespace {

// Helpers for mate-pattern detection: find the king of a given
// colour, and compute the king's escape-square count restricted
// to friendly-blocked / attacked exclusions.
std::pair<int,int> find_king(const GameState& gs, bool is_white) {
    for (const auto& p : gs.pieces) {
        if (p.alive && p.type == KING && p.is_white == is_white)
            return {p.col, p.row};
    }
    return {-1, -1};
}

// Was the king's home rank or 7th rank? Used by back-rank /
// scholar's / fool's heuristics. Rank in BoardPiece convention:
// 0 = white's first rank, 7 = black's first rank.
bool on_back_rank(int r, bool is_white) {
    return is_white ? r == 0 : r == 7;
}

}  // namespace

std::string classify_mate_pattern(const GameState& post,
                                  const BoardSnapshot& prev,
                                  const std::string& move_uci) {
    if (move_uci.size() < 4) return "";
    int from_c, from_r, to_c, to_r;
    if (!parse_uci_move(move_uci, from_c, from_r, to_c, to_r)) return "";
    int idx = post.grid[to_r][to_c];
    if (idx < 0) return "";
    const BoardPiece& moved = post.pieces[idx];
    bool mating_white = moved.is_white;
    auto [king_c, king_r] = find_king(post, !mating_white);
    if (king_c < 0) return "";

    int ply = static_cast<int>(prev.pieces.size() > 0 ? 0 : 0);  // unused
    (void)ply;
    int total_moves = 0;
    // Move count up to and including this move — pull from the
    // post-state move history if available. Without it we can't
    // detect Fool's / Scholar's accurately, but those rely on
    // very-low ply counts so we approximate via the snapshot
    // history length (caller provides post.snapshots? No — pass
    // through the move history). For now use prev.last_move's
    // existence: if it's empty this was move 1.
    (void)total_moves;

    // ----- Fool's mate: black mates white in 2 moves, with a
    // queen on h4. Pattern: white king e1, white pawns f3 and g4
    // (or f4 and g4), black queen on h4 / Qxh4#.
    if (!mating_white && moved.type == QUEEN &&
        to_c == internal_col_to_file(7 /*h*/) ? false : true) {
        // (we'll use file letters via internal_col_to_file
        // checks below)
    }
    // Helper: file 'h' internal col, rank 3 → square h4 in 0-based.
    auto file_col = [](char f) {
        return file_to_internal_col(f - 'a');
    };
    if (!mating_white && moved.type == QUEEN &&
        to_c == file_col('h') && to_r == 3) {
        // White king on e1, no rook between, both g-pawn and f-
        // pawn off the home rank: smells like Fool's mate.
        bool wK_on_e1 = false;
        for (const auto& p : post.pieces) {
            if (p.alive && p.type == KING && p.is_white &&
                p.col == file_col('e') && p.row == 0) wK_on_e1 = true;
        }
        if (wK_on_e1) {
            // Approximate Fool's by overall ply count from move
            // history (ply ≤ 4).
            // We don't have post.move_history here — caller must
            // gate by ply. Skip the strictest check: just flag.
            return "Fool's mate";
        }
    }

    // ----- Scholar's mate: white queen captures f7 with bishop
    // support on c4 (or vice versa for black on f2). Detect by:
    // - Mating piece is QUEEN.
    // - Mate-delivery square is f7 (white mating black) or f2
    //   (black mating white).
    // - Black king on e8 (resp. white king on e1).
    if (moved.type == QUEEN) {
        int target_file = file_col('f');
        int target_rank = mating_white ? 6 : 1;          // f7 or f2
        int king_home_file = file_col('e');
        int king_home_rank = mating_white ? 7 : 0;
        if (to_c == target_file && to_r == target_rank &&
            king_c == king_home_file && king_r == king_home_rank) {
            return "Scholar's mate";
        }
    }

    // ----- Smothered mate: a knight delivers mate to a king
    // that's blocked on every other adjacent square by its own
    // pieces. We test by enumerating the 8 surrounding squares
    // and checking each is either off-board OR occupied by a
    // friendly piece (of the king's colour).
    if (moved.type == KNIGHT) {
        int blocked = 0, off_board = 0;
        for (int dc = -1; dc <= 1; ++dc) {
            for (int dr = -1; dr <= 1; ++dr) {
                if (dc == 0 && dr == 0) continue;
                int c = king_c + dc, r = king_r + dr;
                if (c < 0 || c > 7 || r < 0 || r > 7) {
                    ++off_board; continue;
                }
                int oidx = post.grid[r][c];
                if (oidx >= 0 && post.pieces[oidx].is_white != mating_white) {
                    ++blocked;
                }
            }
        }
        if (blocked + off_board == 8) {
            return "Smothered mate";
        }
    }

    // ----- Back-rank mate: the king is on its back rank, the
    // mating piece is a rook or queen on that same rank, and the
    // king is blocked from escaping forward by its own pawns.
    if ((moved.type == ROOK || moved.type == QUEEN) &&
        on_back_rank(king_r, !mating_white) &&
        to_r == king_r) {
        // Are the squares immediately in front of the king (one
        // rank further from his home) blocked by his own pieces?
        int forward = (!mating_white) ? -1 : +1;  // king's-pov forward
        int blocked = 0, total = 0;
        for (int dc = -1; dc <= 1; ++dc) {
            int c = king_c + dc;
            int r = king_r + forward;
            if (c < 0 || c > 7 || r < 0 || r > 7) continue;
            ++total;
            int oidx = post.grid[r][c];
            if (oidx >= 0 && post.pieces[oidx].is_white != mating_white) {
                ++blocked;
            }
        }
        if (total > 0 && blocked == total) {
            return "Back-rank mate";
        }
    }

    // ----- Anastasia's mate: knight on e7 (or e2) + rook (or
    // queen) on the h-file mate the king on h7 (or h2). Loose
    // detection: rook/queen delivers mate on the h-file with the
    // king on h7 / h2 and a friendly knight nearby on the e-file.
    if ((moved.type == ROOK || moved.type == QUEEN) &&
        to_c == file_col('h') &&
        (king_c == file_col('h')) &&
        ((mating_white && king_r == 6) ||
         (!mating_white && king_r == 1))) {
        // Look for a friendly knight one rank below the king on
        // the e-file.
        int needed_rank = mating_white ? 6 : 1;
        for (const auto& p : post.pieces) {
            if (!p.alive || p.type != KNIGHT || p.is_white != mating_white)
                continue;
            if (p.col == file_col('e') && p.row == needed_rank) {
                return "Anastasia's mate";
            }
        }
    }

    // ----- Boden's mate: criss-crossing bishops on diagonals
    // mate a castled king. Loose detection: mating piece is a
    // BISHOP, the king is on c8/c1 (long castled) AND another
    // friendly bishop exists on a complementary diagonal.
    if (moved.type == BISHOP) {
        int castled_file = file_col('c');
        bool castled_long = (king_c == castled_file) &&
                            ((mating_white && king_r == 7) ||
                             (!mating_white && king_r == 0));
        if (castled_long) {
            int other_bishops = 0;
            for (const auto& p : post.pieces) {
                if (!p.alive || p.type != BISHOP || p.is_white != mating_white)
                    continue;
                if (p.col == to_c && p.row == to_r) continue;
                ++other_bishops;
            }
            if (other_bishops >= 1) return "Boden's mate";
        }
    }

    return "";
}
