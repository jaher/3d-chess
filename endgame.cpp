#include "endgame.h"

namespace {

struct PieceCount {
    int K = 0, Q = 0, R = 0, B = 0, N = 0, P = 0;
    // Bishop square colour bookkeeping for the same-vs-opposite-
    // colour bishop endgame split. true = light square, false =
    // dark; vector size matches B.
    std::vector<bool> bishop_light;
};

PieceCount count_for(const GameState& gs, bool side_white) {
    PieceCount pc;
    for (const auto& p : gs.pieces) {
        if (!p.alive || p.is_white != side_white) continue;
        switch (p.type) {
            case KING:   ++pc.K; break;
            case QUEEN:  ++pc.Q; break;
            case ROOK:   ++pc.R; break;
            case BISHOP:
                ++pc.B;
                // Standard chess square colour: light when (col+row)
                // is even — internal indexing is consistent enough
                // for "same/opposite colour" comparison even if the
                // absolute mapping differs.
                pc.bishop_light.push_back(((p.col + p.row) & 1) == 0);
                break;
            case KNIGHT: ++pc.N; break;
            case PAWN:   ++pc.P; break;
            default:     break;
        }
    }
    return pc;
}

int total_material_points(const PieceCount& w, const PieceCount& b) {
    return 9 * (w.Q + b.Q) + 5 * (w.R + b.R) +
           3 * (w.B + b.B + w.N + b.N) + (w.P + b.P);
}

bool only_king(const PieceCount& p) {
    return p.K == 1 && p.Q == 0 && p.R == 0 && p.B == 0 && p.N == 0 && p.P == 0;
}

bool king_and_pawns_only(const PieceCount& p) {
    return p.Q == 0 && p.R == 0 && p.B == 0 && p.N == 0;
}

}  // namespace

std::string classify_endgame(const GameState& gs) {
    PieceCount w = count_for(gs, true);
    PieceCount b = count_for(gs, false);

    // Game-state sanity: both kings must be on the board for the
    // classifier's "lone king" patterns to mean anything.
    if (w.K != 1 || b.K != 1) return "";

    int total = total_material_points(w, b);
    // Hard gate — anything above this is still middlegame
    // territory. 24 ≈ "no queens + a couple of minors / a rook on
    // each side" or equivalent.
    if (total > 24) return "";

    // ----- Lone-king patterns: classic "you must mate now" labels.
    // Whichever side has the material sits on the left of the slash.
    if (only_king(b)) {
        if (w.Q == 1 && w.R == 0 && w.B == 0 && w.N == 0 && w.P == 0)
            return "Queen versus lone king endgame";
        if (w.Q == 0 && w.R == 1 && w.B == 0 && w.N == 0 && w.P == 0)
            return "Rook versus lone king endgame";
        if (w.Q == 0 && w.R == 0 && w.B == 1 && w.N == 1 && w.P == 0)
            return "Bishop and knight mate";
        if (w.Q == 0 && w.R == 0 && w.B == 2 && w.N == 0 && w.P == 0)
            return "Two bishops mate";
        if (w.Q == 0 && w.R == 0 && w.B == 0 && w.N == 0 && w.P > 0)
            return "King and pawn endgame";
    }
    if (only_king(w)) {
        if (b.Q == 1 && b.R == 0 && b.B == 0 && b.N == 0 && b.P == 0)
            return "Queen versus lone king endgame";
        if (b.Q == 0 && b.R == 1 && b.B == 0 && b.N == 0 && b.P == 0)
            return "Rook versus lone king endgame";
        if (b.Q == 0 && b.R == 0 && b.B == 1 && b.N == 1 && b.P == 0)
            return "Bishop and knight mate";
        if (b.Q == 0 && b.R == 0 && b.B == 2 && b.N == 0 && b.P == 0)
            return "Two bishops mate";
        if (b.Q == 0 && b.R == 0 && b.B == 0 && b.N == 0 && b.P > 0)
            return "King and pawn endgame";
    }

    // ----- Both sides have only kings + pawns — the textbook
    // "king and pawn endgame" lesson positions.
    if (king_and_pawns_only(w) && king_and_pawns_only(b) &&
        (w.P + b.P) > 0) {
        return "King and pawn endgame";
    }

    // ----- Single-piece-class endgames (queens off / rooks only /
    // bishops only / knights only). Pawns can be on either side.
    const bool no_queens = (w.Q + b.Q) == 0;
    if (no_queens) {
        const bool only_rooks   = (w.B + w.N + b.B + b.N) == 0 &&
                                  (w.R + b.R) > 0;
        const bool only_bishops = (w.R + w.N + b.R + b.N) == 0 &&
                                  (w.B + b.B) > 0;
        const bool only_knights = (w.R + w.B + b.R + b.B) == 0 &&
                                  (w.N + b.N) > 0;
        const bool only_minors  = (w.R + b.R) == 0 &&
                                  (w.B + w.N + b.B + b.N) > 0;

        if (only_rooks) {
            return (w.P + b.P) > 0 ? "Rook and pawn endgame"
                                    : "Rook endgame";
        }
        if (only_bishops) {
            // Same-colour vs opposite-colour bishop endgames are
            // wildly different in flavour (opposite-colour bishops
            // is famously drawish), so call it out when both sides
            // have exactly one bishop.
            if (w.B == 1 && b.B == 1 &&
                w.bishop_light.size() == 1 && b.bishop_light.size() == 1) {
                bool same = (w.bishop_light[0] == b.bishop_light[0]);
                return same ? "Same-coloured bishops endgame"
                            : "Opposite-coloured bishops endgame";
            }
            return "Bishop endgame";
        }
        if (only_knights) {
            return "Knight endgame";
        }
        if (only_minors) {
            return "Minor piece endgame";
        }
    }

    // ----- Queen-only endgames (Q+P vs Q+P or Q vs Q).
    if ((w.Q + b.Q) > 0 && (w.R + b.R + w.B + w.N + b.B + b.N) == 0) {
        return "Queen endgame";
    }

    // ----- Generic fallback — material's low enough to call it an
    // endgame even when the piece mix doesn't fit a named family
    // (e.g. R+B vs R+N, Q vs R+N, etc.).
    if (total <= 14) return "Endgame";

    return "";
}
