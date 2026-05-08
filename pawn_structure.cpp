#include "pawn_structure.h"

#include "ai_player.h"      // internal_col_to_file

namespace {

// Per-side pawn metadata indexed by FILE (0..7, a..h). The chess
// codebase uses an "internal" column convention reversed from the
// file index, so we run everything through internal_col_to_file()
// up front to keep this module readable.
struct PawnInfo {
    int count_per_file[8] = {0,0,0,0,0,0,0,0};
    // Most-advanced pawn rank per file (relative to white: rank 0
    // is the home, 7 is promotion). For black the same number
    // means the absolute rank is 7 - rank_advanced.
    int max_rank_per_file[8] = {-1,-1,-1,-1,-1,-1,-1,-1};
    int min_rank_per_file[8] = {8,8,8,8,8,8,8,8};
};

PawnInfo gather(const GameState& gs, bool side_white) {
    PawnInfo info;
    for (const auto& p : gs.pieces) {
        if (!p.alive || p.type != PAWN || p.is_white != side_white) continue;
        int file = internal_col_to_file(p.col);
        ++info.count_per_file[file];
        // Use the BoardPiece's row directly (0 = white's first
        // rank). For "advanced" we want max(row) for white, min(row)
        // for black — but the abstraction is cleaner if we always
        // record max + min and let callers pick.
        if (p.row > info.max_rank_per_file[file]) info.max_rank_per_file[file] = p.row;
        if (p.row < info.min_rank_per_file[file]) info.min_rank_per_file[file] = p.row;
    }
    return info;
}

// Most-advanced own pawn on `file`, in the side-relative sense:
// for white this is the highest rank; for black it's the lowest
// rank converted back to "how far from home". Returns -1 if no
// pawn on the file.
int most_advanced_own(const PawnInfo& info, int file, bool is_white) {
    if (file < 0 || file > 7) return -1;
    if (info.count_per_file[file] == 0) return -1;
    return is_white ? info.max_rank_per_file[file]
                    : (7 - info.min_rank_per_file[file]);
}

// Is `file` an isolani for this side? (No friendly pawn on
// adjacent files.)
bool is_isolated(const PawnInfo& info, int file) {
    if (info.count_per_file[file] == 0) return false;
    bool left  = (file > 0) ? info.count_per_file[file - 1] > 0 : false;
    bool right = (file < 7) ? info.count_per_file[file + 1] > 0 : false;
    return !left && !right;
}

// Returns "white" / "black" / "" depending on whether the position
// has an isolated queen pawn for either side. d-file is index 3.
std::string detect_iqp(const PawnInfo& w, const PawnInfo& b) {
    if (w.count_per_file[3] > 0 && is_isolated(w, 3)) return "white";
    if (b.count_per_file[3] > 0 && is_isolated(b, 3)) return "black";
    return "";
}

// Detect a passed pawn for the named side. A pawn is passed if no
// enemy pawn sits on its file or adjacent files at a square that
// can still block / capture (i.e. ahead of it from the pawn's
// perspective). Returns the file letter (0..7 → 'a'..'h') of the
// most-advanced passed pawn, or -1 if none.
int detect_passed_pawn(const GameState& gs, bool side_white) {
    int best_file = -1;
    int best_advance = -1;
    for (const auto& p : gs.pieces) {
        if (!p.alive || p.type != PAWN || p.is_white != side_white) continue;
        int file = internal_col_to_file(p.col);
        bool blocked = false;
        for (int df = -1; df <= 1 && !blocked; ++df) {
            int f = file + df;
            if (f < 0 || f > 7) continue;
            for (const auto& q : gs.pieces) {
                if (!q.alive || q.type != PAWN || q.is_white == side_white)
                    continue;
                int qf = internal_col_to_file(q.col);
                if (qf != f) continue;
                // "Ahead" check: for white, enemy pawn must be on a
                // higher rank than ours (i.e. q.row > p.row). For
                // black it's q.row < p.row.
                if ((side_white && q.row > p.row) ||
                    (!side_white && q.row < p.row)) {
                    blocked = true;
                    break;
                }
            }
        }
        if (!blocked) {
            int advance = side_white ? p.row : 7 - p.row;
            if (advance > best_advance) {
                best_advance = advance;
                best_file = file;
            }
        }
    }
    return best_file;
}

bool has_doubled_pawn(const PawnInfo& info) {
    for (int f = 0; f < 8; ++f) {
        if (info.count_per_file[f] >= 2) return true;
    }
    return false;
}

// Hanging pawns — a pair of side-by-side friendly pawns on the c
// + d or d + e or another adjacent-file pair, with the OUTSIDE
// files (one past each end of the pair) empty for that side. The
// classic case is c5 + d5 with no friendly pawn on b- or e-files.
bool has_hanging_pawns(const PawnInfo& info) {
    for (int f = 0; f + 1 < 7; ++f) {
        if (info.count_per_file[f]   != 1) continue;
        if (info.count_per_file[f+1] != 1) continue;
        bool left_empty  = (f == 0)   ? true : info.count_per_file[f-1] == 0;
        bool right_empty = (f+1 == 7) ? true : info.count_per_file[f+2] == 0;
        if (left_empty && right_empty) return true;
    }
    return false;
}

const char* file_letter(int file_idx) {
    static const char* names[] = {"a", "b", "c", "d", "e", "f", "g", "h"};
    if (file_idx < 0 || file_idx > 7) return "";
    return names[file_idx];
}

}  // namespace

std::string classify_pawn_structure(const GameState& gs) {
    PawnInfo w = gather(gs, true);
    PawnInfo b = gather(gs, false);

    // ----- Isolated queen pawn (the most famous structural
    // feature; also the one most worth narrating).
    std::string iqp_side = detect_iqp(w, b);
    if (!iqp_side.empty()) {
        return iqp_side == "white"
            ? "White has an isolated queen pawn"
            : "Black has an isolated queen pawn";
    }

    // ----- Hanging pawns (d-file + c-file or e-file pair).
    if (has_hanging_pawns(w)) return "White has hanging pawns";
    if (has_hanging_pawns(b)) return "Black has hanging pawns";

    // ----- Passed pawns. The advanced ones are the dramatic
    // ones — only call out when the pawn is past the 4th rank
    // from its own side (rank index 4 for white, rank index 3 for
    // black).
    int wp = detect_passed_pawn(gs, true);
    int bp = detect_passed_pawn(gs, false);
    if (wp >= 0) {
        // Find the rank to gate the announcement.
        int adv = most_advanced_own(w, wp, true);
        if (adv >= 4) {
            std::string s = "White passed pawn on the ";
            s += file_letter(wp);
            s += " file";
            return s;
        }
    }
    if (bp >= 0) {
        int adv = most_advanced_own(b, bp, false);
        if (adv >= 4) {
            std::string s = "Black passed pawn on the ";
            s += file_letter(bp);
            s += " file";
            return s;
        }
    }

    // ----- Doubled pawns (lower priority — only call out once
    // per side appearance).
    if (has_doubled_pawn(w)) return "White has doubled pawns";
    if (has_doubled_pawn(b)) return "Black has doubled pawns";

    return "";
}
