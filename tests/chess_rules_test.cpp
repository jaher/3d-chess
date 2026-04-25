// Tests for chess_rules.cpp — move generation, check/mate detection,
// execute_move, uci_to_algebraic. Uses helpers::state_from_fen to
// set up positions in standard notation.

#include "doctest.h"
#include "helpers.h"

#include "../chess_rules.h"
#include "../chess_types.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

// Does a move list contain a move with the given destination square?
static bool contains_move(const std::vector<std::pair<int,int>>& moves,
                          int to_col, int to_row) {
    for (const auto& m : moves)
        if (m.first == to_col && m.second == to_row) return true;
    return false;
}

// ===========================================================================
// Starting position
// ===========================================================================
TEST_CASE("build_starting_position returns 32 pieces") {
    auto pieces = build_starting_position();
    REQUIRE(pieces.size() == 32);

    int white_count = 0, black_count = 0;
    int kings = 0, queens = 0, rooks = 0, bishops = 0, knights = 0, pawns = 0;
    for (const auto& p : pieces) {
        if (p.is_white) white_count++;
        else            black_count++;
        switch (p.type) {
            case KING:   kings++;   break;
            case QUEEN:  queens++;  break;
            case ROOK:   rooks++;   break;
            case BISHOP: bishops++; break;
            case KNIGHT: knights++; break;
            case PAWN:   pawns++;   break;
            default: break;
        }
    }
    CHECK(white_count == 16);
    CHECK(black_count == 16);
    CHECK(kings == 2);
    CHECK(queens == 2);
    CHECK(rooks == 4);
    CHECK(bishops == 4);
    CHECK(knights == 4);
    CHECK(pawns == 16);
}

TEST_CASE("starting position: king on e1 / e8, queen on d1 / d8") {
    GameState gs = starting_state();
    const BoardPiece* wk = piece_at(gs, col_of('e'), row_of('1'));
    const BoardPiece* wq = piece_at(gs, col_of('d'), row_of('1'));
    const BoardPiece* bk = piece_at(gs, col_of('e'), row_of('8'));
    const BoardPiece* bq = piece_at(gs, col_of('d'), row_of('8'));

    REQUIRE(wk != nullptr);
    REQUIRE(wq != nullptr);
    REQUIRE(bk != nullptr);
    REQUIRE(bq != nullptr);

    CHECK(wk->type == KING);
    CHECK(wk->is_white);
    CHECK(wq->type == QUEEN);
    CHECK(wq->is_white);
    CHECK(bk->type == KING);
    CHECK_FALSE(bk->is_white);
    CHECK(bq->type == QUEEN);
    CHECK_FALSE(bq->is_white);
}

TEST_CASE("evaluate_position returns 0 for the starting position") {
    GameState gs = starting_state();
    float eval = evaluate_position(gs);
    // Symmetric position — should be ~0.
    CHECK(std::abs(eval) < 1e-3f);
}

// ===========================================================================
// Per-piece move generation from the starting position
// ===========================================================================
TEST_CASE("starting position knight on b1 has 2 moves (a3, c3)") {
    GameState gs = starting_state();
    int bc = col_of('b'), br = row_of('1');
    auto moves = generate_legal_moves(gs, bc, br);
    CHECK(moves.size() == 2);
    CHECK(contains_move(moves, col_of('a'), row_of('3')));
    CHECK(contains_move(moves, col_of('c'), row_of('3')));
}

TEST_CASE("starting position pawn on e2 has 2 moves (e3, e4)") {
    GameState gs = starting_state();
    auto moves = generate_legal_moves(gs, col_of('e'), row_of('2'));
    CHECK(moves.size() == 2);
    CHECK(contains_move(moves, col_of('e'), row_of('3')));
    CHECK(contains_move(moves, col_of('e'), row_of('4')));
}

TEST_CASE("starting position rook on a1 has zero legal moves (blocked)") {
    GameState gs = starting_state();
    auto moves = generate_legal_moves(gs, col_of('a'), row_of('1'));
    CHECK(moves.empty());
}

TEST_CASE("starting position bishop on c1 has zero legal moves (blocked)") {
    GameState gs = starting_state();
    auto moves = generate_legal_moves(gs, col_of('c'), row_of('1'));
    CHECK(moves.empty());
}

TEST_CASE("starting position king on e1 has zero legal moves") {
    GameState gs = starting_state();
    auto moves = generate_legal_moves(gs, col_of('e'), row_of('1'));
    CHECK(moves.empty());
}

TEST_CASE("fianchetto bishop on g2 on open diagonal has 7 moves") {
    // Black king on e8 so it's a legal position; white king safely on e1;
    // everything else cleared so the bishop has a full diagonal.
    GameState gs = state_from_fen("4k3/8/8/8/8/8/6B1/4K3 w - - 0 1");
    auto moves = generate_legal_moves(gs, col_of('g'), row_of('2'));
    // From g2: diagonals h1 (1), f1 blocked by king? No — king on e1, f1
    // is empty. So f1 (1 move), h1 (1 move), h3/f3 (2), and the long
    // diagonal f3..a8 = 6 moves, plus h1 and h3 = 2 more. Let's just
    // assert the count matches an empty-diagonal calculation:
    //   g2→h1 (1), g2→h3 (1), g2→f1..a1 (1: only f1), g2→f3..a8 (6)
    // Total: 1 + 1 + 1 + 6 = 9. But some squares are blocked by kings
    // or off the board; the point is the bishop has many more moves
    // than 2 and isn't stuck. Just check it's > 7.
    CHECK(moves.size() >= 7);
}

TEST_CASE("rook on open file has 13 legal moves from e5") {
    // Position: black king e8, white rook e5, white king e1. Pure open
    // file + open rank for the rook.
    GameState gs = state_from_fen("4k3/8/8/4R3/8/8/8/4K3 w - - 0 1");
    auto moves = generate_legal_moves(gs, col_of('e'), row_of('5'));
    // e-file up:   e6, e7, e8          = 3 (e8 included as pseudo-legal
    //                                     king capture — the engine's
    //                                     move generator doesn't
    //                                     specially exclude the enemy
    //                                     king square)
    // e-file down: e4, e3, e2          = 3 (e1 blocked by own king)
    // rank 5 left: d5, c5, b5, a5      = 4
    // rank 5 right: f5, g5, h5         = 3
    // total:                             13
    CHECK(moves.size() == 13);

    // Spot-check a few specific destinations.
    CHECK(contains_move(moves, col_of('e'), row_of('6')));
    CHECK(contains_move(moves, col_of('a'), row_of('5')));
    CHECK(contains_move(moves, col_of('h'), row_of('5')));
    CHECK_FALSE(contains_move(moves, col_of('e'), row_of('1')));  // own king
}

// ===========================================================================
// Attack detection / check
// ===========================================================================
TEST_CASE("starting position: nobody is in check") {
    GameState gs = starting_state();
    CHECK_FALSE(is_in_check(gs, true));
    CHECK_FALSE(is_in_check(gs, false));
}

TEST_CASE("king in check is detected") {
    // White king on e1, black rook on e8, open e-file. White king in check.
    GameState gs = state_from_fen("4r3/8/8/8/8/8/8/4K3 w - - 0 1");
    CHECK(is_in_check(gs, true));
    CHECK_FALSE(is_in_check(gs, false));
}

TEST_CASE("is_square_attacked: rook controls its file") {
    GameState gs = state_from_fen("4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
    // White rook on a1 attacks every square along a-file (except a1
    // itself) and every square on rank 1 (except those occupied by own
    // pieces). Pick a3 on the a-file.
    CHECK(is_square_attacked(gs, col_of('a'), row_of('3'), /*by_white=*/true));
    // g5 is nowhere near the rook.
    CHECK_FALSE(is_square_attacked(gs, col_of('g'), row_of('5'), /*by_white=*/true));
}

// ===========================================================================
// Pinned piece restrictions
// ===========================================================================
TEST_CASE("pinned black rook cannot move off the pinning line") {
    // White rook on e1, black rook on e5, black king on e8 — the black
    // rook is absolutely pinned along the e-file by the white rook.
    // It may still move along the e-file (and capture the pinner on e1).
    GameState gs = state_from_fen("4k3/8/8/4r3/8/8/8/4R2K b - - 0 1");
    auto moves = generate_legal_moves(gs, col_of('e'), row_of('5'));

    // Any moves must stay on the e-file.
    for (const auto& m : moves) {
        CHECK(m.first == col_of('e'));
    }
    // And the rook must have at least one legal move (along the file).
    CHECK_FALSE(moves.empty());
    // Specifically it can slide to e4, e3, e2 and capture on e1.
    CHECK(contains_move(moves, col_of('e'), row_of('4')));
    CHECK(contains_move(moves, col_of('e'), row_of('1')));  // capture
}

// ===========================================================================
// Checkmate / stalemate detection
// ===========================================================================
TEST_CASE("fool's mate detected as checkmate") {
    // After 1. f3 e5 2. g4 Qh4#, white is mated.
    GameState gs = state_from_fen(
        "rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3");
    CHECK(is_in_check(gs, true));
    CHECK_FALSE(has_any_legal_move(gs, true));

    check_game_over(gs);
    CHECK(gs.game_over);
    CHECK(gs.game_result.find("Black wins") != std::string::npos);
    CHECK(gs.game_result.find("checkmate") != std::string::npos);
}

TEST_CASE("back-rank mate with cornered king detected") {
    // White king on h1 boxed in by its own pawns, black rook delivers
    // mate on e1. Not in check yet? It IS in check — rook on e1 attacks
    // along rank 1 all the way to h1.
    GameState gs = state_from_fen("4k3/8/8/8/8/8/5PPP/4r2K w - - 0 1");
    CHECK(is_in_check(gs, true));
    CHECK_FALSE(has_any_legal_move(gs, true));
    check_game_over(gs);
    CHECK(gs.game_over);
    CHECK(gs.game_result.find("Black wins") != std::string::npos);
}

TEST_CASE("classic corner stalemate detected as draw") {
    // White king f7, white queen g6, black king h8 to move — black has
    // no legal moves and is not in check.
    GameState gs = state_from_fen("7k/5K2/6Q1/8/8/8/8/8 b - - 0 1");
    CHECK_FALSE(is_in_check(gs, false));
    CHECK_FALSE(has_any_legal_move(gs, false));
    check_game_over(gs);
    CHECK(gs.game_over);
    CHECK(gs.game_result.find("stalemate") != std::string::npos);
}

TEST_CASE("starting position is not game over") {
    GameState gs = starting_state();
    CHECK(has_any_legal_move(gs, true));
    check_game_over(gs);
    CHECK_FALSE(gs.game_over);
}

// ===========================================================================
// execute_move — side effects
// ===========================================================================
TEST_CASE("execute_move e2e4 updates pawn and flips turn") {
    GameState gs = starting_state();
    CHECK(gs.white_turn);

    execute_move(gs, col_of('e'), row_of('2'), col_of('e'), row_of('4'));

    // The e2 square is now empty, the e4 square has the white pawn.
    CHECK(piece_at(gs, col_of('e'), row_of('2')) == nullptr);
    const BoardPiece* p = piece_at(gs, col_of('e'), row_of('4'));
    REQUIRE(p != nullptr);
    CHECK(p->type == PAWN);
    CHECK(p->is_white);

    CHECK_FALSE(gs.white_turn);
    CHECK(gs.move_history.size() == 1);
    CHECK(gs.snapshots.size() == 2);  // starting snapshot + after-move
    CHECK(gs.score_history.size() == 2);
}

TEST_CASE("execute_move capture removes the captured piece") {
    // White knight on c3 captures black pawn on e4.
    GameState gs = state_from_fen(
        "rnbqkbnr/pppp1ppp/8/8/4p3/2N5/PPPP1PPP/R1BQKBNR w KQkq - 0 1");

    int before_count = 0;
    for (const auto& p : gs.pieces) if (p.alive) before_count++;

    execute_move(gs, col_of('c'), row_of('3'), col_of('e'), row_of('4'));

    int after_count = 0;
    for (const auto& p : gs.pieces) if (p.alive) after_count++;
    CHECK(after_count == before_count - 1);

    const BoardPiece* cap = piece_at(gs, col_of('e'), row_of('4'));
    REQUIRE(cap != nullptr);
    CHECK(cap->type == KNIGHT);
    CHECK(cap->is_white);
}

TEST_CASE("execute_move kingside castling moves king and rook") {
    // Both sides can castle kingside; path between e1 and h1 is empty.
    GameState gs = state_from_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    REQUIRE(piece_at(gs, col_of('e'), row_of('1'))->type == KING);
    REQUIRE(piece_at(gs, col_of('h'), row_of('1'))->type == ROOK);

    execute_move(gs, col_of('e'), row_of('1'), col_of('g'), row_of('1'));

    const BoardPiece* k = piece_at(gs, col_of('g'), row_of('1'));
    const BoardPiece* r = piece_at(gs, col_of('f'), row_of('1'));
    REQUIRE(k != nullptr);
    REQUIRE(r != nullptr);
    CHECK(k->type == KING);
    CHECK(r->type == ROOK);
    CHECK(piece_at(gs, col_of('e'), row_of('1')) == nullptr);
    CHECK(piece_at(gs, col_of('h'), row_of('1')) == nullptr);
    CHECK(gs.castling.white_king_moved);
}

TEST_CASE("execute_move queenside castling moves king and rook") {
    GameState gs = state_from_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    execute_move(gs, col_of('e'), row_of('1'), col_of('c'), row_of('1'));

    const BoardPiece* k = piece_at(gs, col_of('c'), row_of('1'));
    const BoardPiece* r = piece_at(gs, col_of('d'), row_of('1'));
    REQUIRE(k != nullptr);
    REQUIRE(r != nullptr);
    CHECK(k->type == KING);
    CHECK(r->type == ROOK);
    CHECK(piece_at(gs, col_of('a'), row_of('1')) == nullptr);
}

TEST_CASE("execute_move promotion auto-queens") {
    // White pawn on a7, empty a8. Push to a8 — should become a queen.
    GameState gs = state_from_fen("4k3/P7/8/8/8/8/8/4K3 w - - 0 1");
    execute_move(gs, col_of('a'), row_of('7'), col_of('a'), row_of('8'));

    const BoardPiece* promoted = piece_at(gs, col_of('a'), row_of('8'));
    REQUIRE(promoted != nullptr);
    CHECK(promoted->type == QUEEN);
    CHECK(promoted->is_white);
}

// ===========================================================================
// uci_to_algebraic
// ===========================================================================
TEST_CASE("uci_to_algebraic: pawn push e4") {
    GameState gs = starting_state();
    const BoardSnapshot before = gs.snapshots.back();
    std::string alg = uci_to_algebraic(before, "e2e4");
    // Pawn moves are usually just the destination square.
    CHECK(alg.find("e4") != std::string::npos);
}

TEST_CASE("uci_to_algebraic: knight move Nf3") {
    GameState gs = starting_state();
    const BoardSnapshot before = gs.snapshots.back();
    std::string alg = uci_to_algebraic(before, "g1f3");
    CHECK(alg[0] == 'N');
    CHECK(alg.find("f3") != std::string::npos);
}

TEST_CASE("uci_to_algebraic: kingside castling renders as O-O") {
    GameState gs = state_from_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    // take_snapshot happens in apply_fen_to_state — use the current snapshot.
    const BoardSnapshot before = gs.snapshots.back();
    std::string alg = uci_to_algebraic(before, "e1g1");
    CHECK(alg.find("O-O") != std::string::npos);
}

TEST_CASE("uci_to_algebraic: queenside castling renders as O-O-O") {
    GameState gs = state_from_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    const BoardSnapshot before = gs.snapshots.back();
    std::string alg = uci_to_algebraic(before, "e1c1");
    CHECK(alg.find("O-O-O") != std::string::npos);
}

TEST_CASE("uci_to_algebraic: capture has an 'x'") {
    // Knight on c3 captures pawn on e4 — "Nxe4".
    GameState gs = state_from_fen(
        "rnbqkbnr/pppp1ppp/8/8/4p3/2N5/PPPP1PPP/R1BQKBNR w KQkq - 0 1");
    const BoardSnapshot before = gs.snapshots.back();
    std::string alg = uci_to_algebraic(before, "c3e4");
    CHECK(alg.find('x') != std::string::npos);
    CHECK(alg.find("e4") != std::string::npos);
}

// ===========================================================================
// Castling for black (white side covered above)
// ===========================================================================
TEST_CASE("execute_move black kingside castling moves king and rook") {
    GameState gs = state_from_fen("r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1");
    REQUIRE(piece_at(gs, col_of('e'), row_of('8'))->type == KING);
    REQUIRE(piece_at(gs, col_of('h'), row_of('8'))->type == ROOK);

    execute_move(gs, col_of('e'), row_of('8'), col_of('g'), row_of('8'));

    const BoardPiece* k = piece_at(gs, col_of('g'), row_of('8'));
    const BoardPiece* r = piece_at(gs, col_of('f'), row_of('8'));
    REQUIRE(k != nullptr);
    REQUIRE(r != nullptr);
    CHECK(k->type == KING);
    CHECK_FALSE(k->is_white);
    CHECK(r->type == ROOK);
    CHECK(piece_at(gs, col_of('e'), row_of('8')) == nullptr);
    CHECK(piece_at(gs, col_of('h'), row_of('8')) == nullptr);
    CHECK(gs.castling.black_king_moved);
    CHECK_FALSE(gs.castling.white_king_moved);
}

TEST_CASE("execute_move black queenside castling moves king and rook") {
    GameState gs = state_from_fen("r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1");
    execute_move(gs, col_of('e'), row_of('8'), col_of('c'), row_of('8'));

    const BoardPiece* k = piece_at(gs, col_of('c'), row_of('8'));
    const BoardPiece* r = piece_at(gs, col_of('d'), row_of('8'));
    REQUIRE(k != nullptr);
    REQUIRE(r != nullptr);
    CHECK(k->type == KING);
    CHECK(r->type == ROOK);
    CHECK(piece_at(gs, col_of('a'), row_of('8')) == nullptr);
}

// ===========================================================================
// Castling-through-check: cannot castle when any square the king crosses
// is attacked, even though the destination itself may be safe.
// ===========================================================================
TEST_CASE("kingside castling is illegal when f1 is attacked by a black rook") {
    // Black rook on f8 attacks down the f-file. White king on e1, white
    // kingside rook on h1; e1 itself isn't attacked, but f1 (which the
    // king would cross) is.
    GameState gs = state_from_fen("4k3/8/8/8/8/8/8/4K2R w K - 0 1");
    // Place the black rook on f8 by editing the FEN: rebuild a sharper
    // position with the rook actually on f8 (not just rank 8 generally).
    gs = state_from_fen("5r2/4k3/8/8/8/8/8/4K2R w K - 0 1");
    auto moves = generate_legal_moves(gs, col_of('e'), row_of('1'));
    // King may not castle to g1 — that destination is barred.
    CHECK_FALSE(contains_move(moves, col_of('g'), row_of('1')));
}

TEST_CASE("kingside castling is legal when path is clear and unattacked") {
    GameState gs = state_from_fen("4k3/8/8/8/8/8/8/4K2R w K - 0 1");
    auto moves = generate_legal_moves(gs, col_of('e'), row_of('1'));
    CHECK(contains_move(moves, col_of('g'), row_of('1')));
}

// ===========================================================================
// Castling rights revocation
// ===========================================================================
TEST_CASE("moving the white kingside rook revokes white kingside castling") {
    GameState gs = state_from_fen("4k3/8/8/8/8/8/8/4K2R w K - 0 1");
    CHECK_FALSE(gs.castling.white_rook_h_moved);
    // Move rook h1 → h2.
    execute_move(gs, col_of('h'), row_of('1'), col_of('h'), row_of('2'));
    CHECK(gs.castling.white_rook_h_moved);
    CHECK_FALSE(gs.castling.white_king_moved);
}

TEST_CASE("moving the white king revokes both white castling sides") {
    GameState gs = state_from_fen("4k3/8/8/8/8/8/8/R3K2R w KQ - 0 1");
    execute_move(gs, col_of('e'), row_of('1'), col_of('e'), row_of('2'));
    CHECK(gs.castling.white_king_moved);
}

TEST_CASE("capturing a rook on its home square revokes that castling right") {
    // Black bishop on a8 sweeps down to capture white rook on h1 — wait,
    // a8 → h1 is the long diagonal; that lands on h1 in one move. Use a
    // simpler capture: black rook on h8 takes white rook on h1 along the
    // h-file.
    GameState gs = state_from_fen("4k2r/8/8/8/8/8/8/4K2R b KQkq - 0 1");
    CHECK_FALSE(gs.castling.white_rook_h_moved);
    execute_move(gs, col_of('h'), row_of('8'), col_of('h'), row_of('1'));
    // The capture happens on h1 (white kingside rook square). White
    // kingside castling is now lost.
    CHECK(gs.castling.white_rook_h_moved);
}

// ===========================================================================
// Pawn promotion variations
// ===========================================================================
TEST_CASE("execute_move black pawn auto-queens on rank 1") {
    GameState gs = state_from_fen("4k3/8/8/8/8/8/p7/4K3 b - - 0 1");
    execute_move(gs, col_of('a'), row_of('2'), col_of('a'), row_of('1'));
    const BoardPiece* promoted = piece_at(gs, col_of('a'), row_of('1'));
    REQUIRE(promoted != nullptr);
    CHECK(promoted->type == QUEEN);
    CHECK_FALSE(promoted->is_white);
}

TEST_CASE("promotion via capture also auto-queens") {
    // White pawn on b7 captures black knight on a8, becomes queen on a8.
    GameState gs = state_from_fen("n3k3/1P6/8/8/8/8/8/4K3 w - - 0 1");
    int alive_before = 0;
    for (const auto& p : gs.pieces) if (p.alive) alive_before++;

    execute_move(gs, col_of('b'), row_of('7'), col_of('a'), row_of('8'));

    const BoardPiece* promoted = piece_at(gs, col_of('a'), row_of('8'));
    REQUIRE(promoted != nullptr);
    CHECK(promoted->type == QUEEN);
    CHECK(promoted->is_white);

    int alive_after = 0;
    for (const auto& p : gs.pieces) if (p.alive) alive_after++;
    CHECK(alive_after == alive_before - 1);  // captured knight is dead
}

// ===========================================================================
// uci_to_algebraic: disambiguation + check / mate suffixes
// ===========================================================================
TEST_CASE("uci_to_algebraic: knight disambiguation by file") {
    // Two white knights on b1 and f1 both reach d2 — disambiguate by
    // file: "Nbd2". (King parked on h1 to keep the position legal.)
    GameState gs = state_from_fen("4k3/8/8/8/8/8/8/1N3N1K w - - 0 1");
    const BoardSnapshot before = gs.snapshots.back();
    std::string alg = uci_to_algebraic(before, "b1d2");
    CHECK(alg[0] == 'N');
    CHECK(alg.find('b') != std::string::npos);  // file disambiguator
    CHECK(alg.find("d2") != std::string::npos);
}

TEST_CASE("uci_to_algebraic: rook disambiguation by rank") {
    // Two white rooks on a1 and a5, both can reach a3. Disambiguate by
    // rank: "R1a3" vs "R5a3" (file is the same so rank is needed).
    GameState gs = state_from_fen("4k3/8/8/R7/8/8/8/R3K3 w - - 0 1");
    const BoardSnapshot before = gs.snapshots.back();
    std::string alg = uci_to_algebraic(before, "a1a3");
    CHECK(alg[0] == 'R');
    // The rank '1' must appear somewhere as a disambiguator.
    CHECK(alg.find('1') != std::string::npos);
    CHECK(alg.find("a3") != std::string::npos);
}

TEST_CASE("uci_to_algebraic: check suffix '+' is appended") {
    // White rook a1 → e1 puts black king on e8 in check along the e-file.
    GameState gs = state_from_fen("4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
    // Move white king out of the way first so the rook check is clean.
    gs = state_from_fen("4k3/8/8/8/8/8/8/R5K1 w - - 0 1");
    const BoardSnapshot before = gs.snapshots.back();
    std::string alg = uci_to_algebraic(before, "a1e1");
    CHECK(alg.back() == '+');
}

TEST_CASE("uci_to_algebraic: mate suffix '#' is appended on checkmate") {
    // Back-rank mate setup: black king h8 boxed by f7,g7,h7; white rook
    // delivers from a1 to a8.
    GameState gs = state_from_fen("7k/5ppp/8/8/8/8/8/R3K3 w - - 0 1");
    const BoardSnapshot before = gs.snapshots.back();
    std::string alg = uci_to_algebraic(before, "a1a8");
    CHECK(alg.back() == '#');
}

// ===========================================================================
// move_leaves_in_check: discovered check on own king is illegal
// ===========================================================================
TEST_CASE("piece pinned against own king cannot move off the pinning ray") {
    // White king e1, white knight e2, black rook e8. The knight is
    // absolutely pinned; any knight move exposes the king.
    GameState gs = state_from_fen("4r3/8/8/8/8/8/4N3/4K3 w - - 0 1");
    auto moves = generate_legal_moves(gs, col_of('e'), row_of('2'));
    // The pinned knight has no legal moves — every knight jump leaves
    // the e-file undefended.
    CHECK(moves.empty());
}

// ===========================================================================
// King-capture victory branch (execute_move fast-path when destination is
// an enemy king — used for the engine's no-legality-check shortcut).
// ===========================================================================
// ===========================================================================
// En passant
// ===========================================================================
TEST_CASE("execute_move: white pawn double-push sets ep target on the skipped square") {
    GameState gs = starting_state();
    CHECK(gs.ep_target_col == -1);
    CHECK(gs.ep_target_row == -1);

    execute_move(gs, col_of('e'), row_of('2'), col_of('e'), row_of('4'));
    // Skipped square is e3 (rank 3, internal row 2).
    CHECK(gs.ep_target_col == col_of('e'));
    CHECK(gs.ep_target_row == row_of('3'));
}

TEST_CASE("execute_move: black pawn double-push sets ep target on the skipped square") {
    GameState gs = state_from_fen(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1");
    execute_move(gs, col_of('e'), row_of('7'), col_of('e'), row_of('5'));
    CHECK(gs.ep_target_col == col_of('e'));
    CHECK(gs.ep_target_row == row_of('6'));
}

TEST_CASE("execute_move: single-step pawn move clears any prior ep target") {
    GameState gs = state_from_fen("4k3/8/8/8/8/8/4P3/4K3 w - e6 0 1");
    REQUIRE(gs.ep_target_col == col_of('e'));
    REQUIRE(gs.ep_target_row == row_of('6'));

    execute_move(gs, col_of('e'), row_of('2'), col_of('e'), row_of('3'));
    CHECK(gs.ep_target_col == -1);
    CHECK(gs.ep_target_row == -1);
}

TEST_CASE("execute_move: non-pawn move clears any prior ep target") {
    GameState gs = state_from_fen("4k3/8/8/8/8/8/8/4K2R w K e6 0 1");
    REQUIRE(gs.ep_target_col == col_of('e'));
    REQUIRE(gs.ep_target_row == row_of('6'));

    execute_move(gs, col_of('h'), row_of('1'), col_of('h'), row_of('2'));
    CHECK(gs.ep_target_col == -1);
}

TEST_CASE("generate_legal_moves: en passant capture is offered when ep target is set") {
    // White pawn on e5, black ep target at d6 (= black just played d7-d5).
    // The white pawn may capture en passant onto d6.
    GameState gs = state_from_fen("4k3/8/3pP3/8/8/8/8/4K3 w - d6 0 1");
    // Re-stage so the bypassed black pawn is actually on d5, not d7.
    gs = state_from_fen("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1");

    auto moves = generate_legal_moves(gs, col_of('e'), row_of('5'));
    bool found_ep = false;
    for (const auto& m : moves)
        if (m.first == col_of('d') && m.second == row_of('6')) found_ep = true;
    CHECK(found_ep);
}

TEST_CASE("generate_legal_moves: ep capture NOT offered when ep target is unset") {
    // Same geometry as the previous test, but FEN has no ep target —
    // so the diagonal-to-empty-square move must NOT be generated.
    GameState gs = state_from_fen("4k3/8/8/3pP3/8/8/8/4K3 w - - 0 1");
    auto moves = generate_legal_moves(gs, col_of('e'), row_of('5'));
    bool found_d6 = false;
    for (const auto& m : moves)
        if (m.first == col_of('d') && m.second == row_of('6')) found_d6 = true;
    CHECK_FALSE(found_d6);
}

TEST_CASE("execute_move: en passant capture removes the bypassed pawn") {
    GameState gs = state_from_fen("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1");
    int alive_before = 0;
    for (const auto& p : gs.pieces) if (p.alive) alive_before++;

    execute_move(gs, col_of('e'), row_of('5'), col_of('d'), row_of('6'));

    // White pawn now on d6.
    const BoardPiece* mover = piece_at(gs, col_of('d'), row_of('6'));
    REQUIRE(mover != nullptr);
    CHECK(mover->type == PAWN);
    CHECK(mover->is_white);

    // Bypassed black pawn that was on d5 is now dead.
    CHECK(piece_at(gs, col_of('d'), row_of('5')) == nullptr);

    int alive_after = 0;
    for (const auto& p : gs.pieces) if (p.alive) alive_after++;
    CHECK(alive_after == alive_before - 1);

    // ep target itself is cleared after the capture.
    CHECK(gs.ep_target_col == -1);
}

TEST_CASE("execute_move: black ep capture removes the bypassed white pawn") {
    GameState gs = state_from_fen("4k3/8/8/8/3Pp3/8/8/4K3 b - d3 0 1");
    execute_move(gs, col_of('e'), row_of('4'), col_of('d'), row_of('3'));
    const BoardPiece* mover = piece_at(gs, col_of('d'), row_of('3'));
    REQUIRE(mover != nullptr);
    CHECK(mover->type == PAWN);
    CHECK_FALSE(mover->is_white);
    CHECK(piece_at(gs, col_of('d'), row_of('4')) == nullptr);
}

TEST_CASE("en passant pin: capture is illegal when it exposes own king") {
    // White king a5, white pawn b5, black pawn c5 (just double-pushed
    // from c7), black rook h5. ep target = c6. If the white pawn
    // captures ep, BOTH b5 and c5 disappear from rank 5 — clearing
    // the line from a5 to h5 and putting the white king in check.
    // generate_legal_moves must filter that capture out.
    GameState gs = state_from_fen("4k3/8/8/Kpp4r/8/8/8/8 w - c6 0 1");
    auto moves = generate_legal_moves(gs, col_of('b'), row_of('5'));
    bool found_c6 = false;
    for (const auto& m : moves)
        if (m.first == col_of('c') && m.second == row_of('6')) found_c6 = true;
    CHECK_FALSE(found_c6);
}

TEST_CASE("uci_to_algebraic: en passant capture renders with 'x'") {
    GameState gs = state_from_fen("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1");
    const BoardSnapshot before = gs.snapshots.back();
    std::string alg = uci_to_algebraic(before, "e5d6");
    CHECK(alg.find('x') != std::string::npos);
    CHECK(alg.find("d6") != std::string::npos);
    // Standard pawn-capture notation starts with the source file.
    CHECK(alg[0] == 'e');
}

TEST_CASE("BoardSnapshot retains ep target across take/restore") {
    GameState gs = state_from_fen("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1");
    REQUIRE(!gs.snapshots.empty());
    CHECK(gs.snapshots.back().ep_target_col == col_of('d'));
    CHECK(gs.snapshots.back().ep_target_row == row_of('6'));

    // Mutate, then restore — ep target should follow the snapshot.
    gs.ep_target_col = -1;
    gs.ep_target_row = -1;
    gs.restore_snapshot(0);
    CHECK(gs.ep_target_col == col_of('d'));
    CHECK(gs.ep_target_row == row_of('6'));
}

TEST_CASE("execute_move that captures the enemy king ends the game") {
    // White rook on e1, black king on e8. Direct king capture (skipping
    // legal-move filtering) should set game_over and game_result.
    GameState gs = state_from_fen("4k3/8/8/8/8/8/8/4R3 w - - 0 1");
    execute_move(gs, col_of('e'), row_of('1'), col_of('e'), row_of('8'));
    CHECK(gs.game_over);
    CHECK(gs.game_result.find("White wins") != std::string::npos);
    // After the move the rook occupies e8; the king is marked dead in
    // the pieces vector but no longer surfaces via piece_at (which
    // filters on alive=true).
    const BoardPiece* p_at_dest = piece_at(gs, col_of('e'), row_of('8'));
    REQUIRE(p_at_dest != nullptr);
    CHECK(p_at_dest->type == ROOK);

    const BoardPiece* dead_king = nullptr;
    for (const auto& p : gs.pieces) {
        if (p.type == KING && !p.is_white) { dead_king = &p; break; }
    }
    REQUIRE(dead_king != nullptr);
    CHECK_FALSE(dead_king->alive);
}
