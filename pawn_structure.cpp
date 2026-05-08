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

namespace {

// Helper: is there a friendly pawn at exactly (file, rank)?
// File / rank in 0..7; rank 0 = white's home rank in BoardPiece
// row indexing.
bool pawn_at(const GameState& gs, int file, int rank, bool side_white) {
    for (const auto& p : gs.pieces) {
        if (!p.alive || p.type != PAWN || p.is_white != side_white) continue;
        if (internal_col_to_file(p.col) == file && p.row == rank) return true;
    }
    return false;
}

// Helper: any friendly pawn on the given file? Convenience wrapper.
bool any_pawn_on_file(const GameState& gs, int file, bool side_white) {
    for (const auto& p : gs.pieces) {
        if (!p.alive || p.type != PAWN || p.is_white != side_white) continue;
        if (internal_col_to_file(p.col) == file) return true;
    }
    return false;
}

}  // namespace

std::string classify_pawn_family(const GameState& gs) {
    // ----- Maroczy bind: white pawns on c4 + e4, black has no
    // d-pawn (or it's on d6). This is a Sicilian / English
    // structure that strongly restricts black's …d5 break.
    if (pawn_at(gs, 2, 3, true) && pawn_at(gs, 4, 3, true) &&
        !pawn_at(gs, 3, 6, false) /* black d-pawn not on d7 */ &&
        !pawn_at(gs, 3, 4, false) /* not on d5 either */) {
        return "Maroczy bind";
    }

    // ----- Stonewall (white): pawns on c3 + d4 + e3 + f4. Black
    // mirror would be c6+d5+e6+f5 — same name, just from the
    // other side.
    if (pawn_at(gs, 2, 2, true) && pawn_at(gs, 3, 3, true) &&
        pawn_at(gs, 4, 2, true) && pawn_at(gs, 5, 3, true)) {
        return "White has a Stonewall structure";
    }
    if (pawn_at(gs, 2, 5, false) && pawn_at(gs, 3, 4, false) &&
        pawn_at(gs, 4, 5, false) && pawn_at(gs, 5, 4, false)) {
        return "Black has a Stonewall structure";
    }

    // ----- Hedgehog (black): pawns on a6, b6, d6, e6 — the
    // signature low-profile setup. The defining feature is the
    // restrained d6/e6 + queenside fianchetto pawns; the
    // standard Hedgehog has a6 + b6 + d6 + e6.
    if (pawn_at(gs, 0, 5, false) && pawn_at(gs, 1, 5, false) &&
        pawn_at(gs, 3, 5, false) && pawn_at(gs, 4, 5, false)) {
        return "Black has a Hedgehog structure";
    }
    // Mirror for white.
    if (pawn_at(gs, 0, 2, true) && pawn_at(gs, 1, 2, true) &&
        pawn_at(gs, 3, 2, true) && pawn_at(gs, 4, 2, true)) {
        return "White has a Hedgehog structure";
    }

    // ----- Benoni (black): white pawn on d5, black pawn on c5.
    // The locked d-pawn imbalance is the signature.
    if (pawn_at(gs, 3, 4, true) && pawn_at(gs, 2, 4, false) &&
        !pawn_at(gs, 3, 6, false)) {
        return "Benoni structure";
    }

    // ----- Carlsbad (white): pawns on a2, b2, c3, d4, e3, f2,
    // g2, h2, with no e-file or c-file capture having occurred —
    // signature pawn skeleton from the Queen's Gambit Declined
    // Exchange Variation. Loose check: white has c3 + d4 + e3
    // + no c4 or e4 pawn. Black mirror: c6 + d5 + e6.
    if (pawn_at(gs, 2, 2, true) && pawn_at(gs, 3, 3, true) &&
        pawn_at(gs, 4, 2, true) &&
        !pawn_at(gs, 2, 3, true) && !pawn_at(gs, 4, 3, true) &&
        pawn_at(gs, 2, 5, false) && pawn_at(gs, 3, 4, false) &&
        pawn_at(gs, 4, 5, false)) {
        return "Carlsbad structure";
    }

    // ----- French chain: white d4 + e5, black d5 + e6 — the
    // "blocked" French Advance position.
    if (pawn_at(gs, 3, 3, true) && pawn_at(gs, 4, 4, true) &&
        pawn_at(gs, 3, 4, false) && pawn_at(gs, 4, 5, false)) {
        return "French pawn chain";
    }

    // ----- Caro-Kann chain: white e5 (or e4), black has c6+d5+e6.
    // The defining feature is the d5/c6/e6 triangle on black's
    // side with white's e-pawn on e5.
    if (pawn_at(gs, 4, 4, true) &&
        pawn_at(gs, 2, 5, false) && pawn_at(gs, 3, 4, false) &&
        pawn_at(gs, 4, 5, false)) {
        return "Caro-Kann pawn chain";
    }

    // ----- King's Indian chain: white pawns on c4 + d5 + e4,
    // black pawn on e5. Locked centre is the hallmark.
    if (pawn_at(gs, 2, 3, true) && pawn_at(gs, 3, 4, true) &&
        pawn_at(gs, 4, 3, true) && pawn_at(gs, 4, 4, false)) {
        return "King's Indian pawn chain";
    }

    // ----- Closed Sicilian: white pawns on c3+d3+e4 (or c2+d3+e4),
    // black on c5+d6+e5 — locked centre with both sides poised
    // for slow-burn flank action.
    if (pawn_at(gs, 4, 3, true) && pawn_at(gs, 3, 2, true) &&
        pawn_at(gs, 2, 4, false) && pawn_at(gs, 3, 5, false) &&
        pawn_at(gs, 4, 4, false)) {
        return "Closed Sicilian structure";
    }
    (void)any_pawn_on_file;
    return "";
}
