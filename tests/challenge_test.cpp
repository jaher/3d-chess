// Tests for challenge.cpp — the FEN parser used both by challenge
// mode loading and (via helpers::state_from_fen) as the shared
// fixture for every other test file (parse_fen here is hand-rolled
// against piece coordinates, not via state_from_fen, so the parser
// isn't tested against itself), plus the tactic-detection helpers
// (move_is_fork / move_is_pin / find_tactic_moves) and the on-disk
// Challenge file loader.

#include "doctest.h"
#include "helpers.h"

#include "../challenge.h"
#include "../chess_types.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Test fixture: write a string to a unique path under /tmp so the on-disk
// Challenge loader has something to read. The destructor unlinks the
// file on scope exit so the test directory stays clean even on failure.
// ---------------------------------------------------------------------------
struct ScopedTempFile {
    std::string path;
    ScopedTempFile(const std::string& contents, const char* suffix = ".md") {
        char tmpl[] = "/tmp/3dchess_test_XXXXXX";
        int fd = mkstemp(tmpl);
        REQUIRE(fd >= 0);
        ::close(fd);
        path = std::string(tmpl) + suffix;
        // mkstemp created tmpl (no suffix); rename so the loader's
        // ".md" extension filter in list_challenge_files matches.
        std::rename(tmpl, path.c_str());
        std::ofstream out(path);
        out << contents;
    }
    ~ScopedTempFile() { ::unlink(path.c_str()); }
};

// Make a unique temporary directory, optionally pre-populating it with
// .md files. The destructor removes everything inside.
struct ScopedTempDir {
    std::string path;
    std::vector<std::string> created;
    ScopedTempDir() {
        char tmpl[] = "/tmp/3dchess_test_dir_XXXXXX";
        REQUIRE(mkdtemp(tmpl) != nullptr);
        path = tmpl;
    }
    void write_file(const std::string& name, const std::string& contents) {
        std::string full = path + "/" + name;
        std::ofstream out(full);
        out << contents;
        created.push_back(full);
    }
    ~ScopedTempDir() {
        for (const auto& f : created) ::unlink(f.c_str());
        ::rmdir(path.c_str());
    }
};

// Search `parsed` for a piece on (col, row). Returns nullptr on miss.
static const BoardPiece* parsed_piece_at(const ParsedFEN& p, int col, int row) {
    for (const auto& piece : p.pieces) {
        if (piece.col == col && piece.row == row) return &piece;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Starting position
// ---------------------------------------------------------------------------
TEST_CASE("parse_fen: standard starting position") {
    ParsedFEN p = parse_fen(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

    REQUIRE(p.valid);
    CHECK(p.pieces.size() == 32);
    CHECK(p.white_turn);

    // Castling rights all present.
    CHECK_FALSE(p.castling.white_king_moved);
    CHECK_FALSE(p.castling.black_king_moved);
    CHECK_FALSE(p.castling.white_rook_a_moved);
    CHECK_FALSE(p.castling.white_rook_h_moved);
    CHECK_FALSE(p.castling.black_rook_a_moved);
    CHECK_FALSE(p.castling.black_rook_h_moved);

    // Spot-check a few pieces at known (col, row) coordinates.
    // Internal col 7 = a-file, col 0 = h-file.
    const BoardPiece* wk = parsed_piece_at(p, /*e*/ 7 - 4, /*rank 1*/ 0);
    REQUIRE(wk != nullptr);
    CHECK(wk->type == KING);
    CHECK(wk->is_white);

    const BoardPiece* bq = parsed_piece_at(p, /*d*/ 7 - 3, /*rank 8*/ 7);
    REQUIRE(bq != nullptr);
    CHECK(bq->type == QUEEN);
    CHECK_FALSE(bq->is_white);

    const BoardPiece* wp_e2 = parsed_piece_at(p, 7 - 4, 1);
    REQUIRE(wp_e2 != nullptr);
    CHECK(wp_e2->type == PAWN);
    CHECK(wp_e2->is_white);
}

TEST_CASE("parse_fen: black to move with empty side-to-move string defaults to white") {
    // "w" / "b" field missing — parse_fen treats empty side as white_turn=true.
    ParsedFEN p = parse_fen("8/8/8/8/8/8/8/4K3");
    // Only 1 piece (the king) — valid because pieces.size() > 0.
    REQUIRE(p.valid);
    CHECK(p.pieces.size() == 1);
    CHECK(p.white_turn);
}

TEST_CASE("parse_fen: explicit black to move") {
    ParsedFEN p = parse_fen("4k3/8/8/8/8/8/8/4K3 b - - 0 1");
    REQUIRE(p.valid);
    CHECK_FALSE(p.white_turn);
    CHECK(p.pieces.size() == 2);
}

TEST_CASE("parse_fen: partial castling rights") {
    // Only white kingside available.
    ParsedFEN p = parse_fen("4k3/8/8/8/8/8/8/4K2R w K - 0 1");
    REQUIRE(p.valid);
    CHECK_FALSE(p.castling.white_king_moved);
    CHECK(p.castling.white_rook_a_moved);     // Q not present
    CHECK_FALSE(p.castling.white_rook_h_moved); // K present
    CHECK(p.castling.black_king_moved);        // no k, no q
}

TEST_CASE("parse_fen: empty string is invalid") {
    ParsedFEN p = parse_fen("");
    CHECK_FALSE(p.valid);
}

TEST_CASE("parse_fen: position with no pieces is invalid") {
    // All-empty placement — no BoardPieces produced, so valid = false.
    ParsedFEN p = parse_fen("8/8/8/8/8/8/8/8 w - - 0 1");
    CHECK_FALSE(p.valid);
}

TEST_CASE("parse_fen: en passant target is read into ep_target_col/row") {
    // White just played e2-e4; black's ep target square is e3.
    ParsedFEN p = parse_fen(
        "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1");
    REQUIRE(p.valid);
    // Internal col 7 = a-file → e-file = 7 - 4 = 3. Rank '3' → row 2.
    CHECK(p.ep_target_col == 3);
    CHECK(p.ep_target_row == 2);
}

TEST_CASE("parse_fen: '-' ep field leaves target unset") {
    ParsedFEN p = parse_fen("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    REQUIRE(p.valid);
    CHECK(p.ep_target_col == -1);
    CHECK(p.ep_target_row == -1);
}

TEST_CASE("parse_fen: malformed ep field ('z9') leaves target unset") {
    ParsedFEN p = parse_fen("4k3/8/8/8/8/8/8/4K3 w - z9 0 1");
    REQUIRE(p.valid);
    CHECK(p.ep_target_col == -1);
    CHECK(p.ep_target_row == -1);
}

TEST_CASE("parse_fen: piece count for a known puzzle position") {
    // Philidor's mate pattern starting position — just a piece count
    // sanity check, not the exact mate puzzle.
    ParsedFEN p = parse_fen("6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1");
    REQUIRE(p.valid);
    // 2 kings + 6 pawns + 1 rook = 9 pieces.
    CHECK(p.pieces.size() == 9);
}

// ===========================================================================
// is_tactic_type
// ===========================================================================
TEST_CASE("is_tactic_type accepts find_forks and find_pins only") {
    CHECK(is_tactic_type("find_forks"));
    CHECK(is_tactic_type("find_pins"));
    CHECK_FALSE(is_tactic_type("mate_in_1"));
    CHECK_FALSE(is_tactic_type("mate_in_2"));
    CHECK_FALSE(is_tactic_type(""));
    CHECK_FALSE(is_tactic_type("find_forks "));   // trailing space
    CHECK_FALSE(is_tactic_type("FIND_FORKS"));    // case-sensitive
}

// ===========================================================================
// move_is_fork
// ===========================================================================
TEST_CASE("move_is_fork: knight forking king and queen") {
    // White knight on f7 attacks d6, d8, e5, g5, h6, h8 — which means
    // it forks the black king on h8 and the black queen on d8.
    GameState gs = state_from_fen("3q3k/5N2/8/8/8/8/8/4K3 b - - 0 1");
    CHECK(move_is_fork(gs, col_of('f'), row_of('7')));
}

TEST_CASE("move_is_fork: knight attacking only one enemy is not a fork") {
    // Black king on e8, white knight on c3, black pawn on b5 — knight
    // attacks b5 only (and a4, d5, e4, etc. which are empty).
    GameState gs = state_from_fen("4k3/8/8/1p6/8/2N5/8/4K3 w - - 0 1");
    CHECK_FALSE(move_is_fork(gs, col_of('c'), row_of('3')));
}

TEST_CASE("move_is_fork: empty square returns false") {
    GameState gs = state_from_fen("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    CHECK_FALSE(move_is_fork(gs, col_of('d'), row_of('4')));
}

TEST_CASE("move_is_fork: out-of-bounds coordinates return false") {
    GameState gs = state_from_fen("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    CHECK_FALSE(move_is_fork(gs, -1, 0));
    CHECK_FALSE(move_is_fork(gs, 0, 8));
    CHECK_FALSE(move_is_fork(gs, 8, 0));
}

// ===========================================================================
// move_is_pin
// ===========================================================================
TEST_CASE("move_is_pin: rook absolutely pins knight to king") {
    // White rook on e1, black knight on e5, black king on e8. The rook
    // sees the knight first along the e-file with the king behind it —
    // king (value 100) > knight (3), so move_is_pin reports a pin.
    GameState gs = state_from_fen("4k3/8/8/4n3/8/8/8/4R2K w - - 0 1");
    CHECK(move_is_pin(gs, col_of('e'), row_of('1')));
}

TEST_CASE("move_is_pin: relative pin where back piece is more valuable") {
    // White bishop on a1, black knight on c3, black rook on e5. Bishop
    // sees knight (3) with rook (5) behind it on the same diagonal
    // a1-h8 — rook value > knight value → relative pin reported.
    GameState gs = state_from_fen("4k3/8/8/4r3/8/2n5/8/B3K3 w - - 0 1");
    CHECK(move_is_pin(gs, col_of('a'), row_of('1')));
}

TEST_CASE("move_is_pin: not a pin when far piece is less valuable") {
    // White bishop a1, black rook c3, black knight e5 — same diagonal,
    // but now the cheap piece (knight=3) is behind the expensive one
    // (rook=5). 3 < 5, so the helper does NOT report a pin.
    GameState gs = state_from_fen("4k3/8/8/4n3/8/2r5/8/B3K3 w - - 0 1");
    CHECK_FALSE(move_is_pin(gs, col_of('a'), row_of('1')));
}

TEST_CASE("move_is_pin: knight is never a pinner") {
    // Even if the geometry would line up, only B/R/Q can pin.
    GameState gs = state_from_fen("4k3/8/8/4n3/8/8/8/4N2K w - - 0 1");
    CHECK_FALSE(move_is_pin(gs, col_of('e'), row_of('1')));
}

TEST_CASE("move_is_pin: only one enemy on the ray means no pin") {
    // White rook on e1, black knight on e5, no king behind — only one
    // enemy along the file, so nothing is pinned.
    GameState gs = state_from_fen("k7/8/8/4n3/8/8/8/4R2K w - - 0 1");
    CHECK_FALSE(move_is_pin(gs, col_of('e'), row_of('1')));
}

TEST_CASE("move_is_pin: own piece blocks the ray before any enemy") {
    // White rook on e1, white pawn on e3, black knight on e5, black
    // king on e8. The rook's e-file sight stops at the white pawn, so
    // it doesn't even see the knight — no pin reported.
    GameState gs = state_from_fen("4k3/8/8/4n3/8/4P3/8/4R2K w - - 0 1");
    CHECK_FALSE(move_is_pin(gs, col_of('e'), row_of('1')));
}

// ===========================================================================
// find_tactic_moves
// ===========================================================================
TEST_CASE("find_tactic_moves: returns empty list for unknown tactic type") {
    GameState gs = state_from_fen("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    auto moves = find_tactic_moves(gs, /*white_side=*/true, "mate_in_1");
    CHECK(moves.empty());
    moves = find_tactic_moves(gs, true, "");
    CHECK(moves.empty());
}

TEST_CASE("find_tactic_moves: enumerates a knight fork move") {
    // White knight on g5 with black king h8 and black queen e6. After
    // Nf7+, the knight on f7 attacks both h8 (king) and d8 — wait, e6
    // is the queen, not d8. Let's place the queen on d8 directly so
    // the geometry is clean: the knight move g5→f7 forks h8 and d8.
    GameState gs = state_from_fen("3q3k/8/8/6N1/8/8/8/4K3 w - - 0 1");
    auto moves = find_tactic_moves(gs, /*white_side=*/true, "find_forks");

    // The fork move is g5f7. Other knight moves don't create a fork.
    bool found_g5f7 = false;
    for (const auto& m : moves) if (m == "g5f7") found_g5f7 = true;
    CHECK(found_g5f7);
}

TEST_CASE("find_tactic_moves: position with no available motif returns empty") {
    // Just two kings — no possible forks or pins.
    GameState gs = state_from_fen("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    auto forks = find_tactic_moves(gs, true,  "find_forks");
    auto pins  = find_tactic_moves(gs, true,  "find_pins");
    CHECK(forks.empty());
    CHECK(pins.empty());
}

// ===========================================================================
// load_challenge — file I/O
// ===========================================================================
TEST_CASE("load_challenge: nonexistent path yields empty Challenge") {
    Challenge ch = load_challenge("/tmp/3dchess_definitely_does_not_exist_xyz.md");
    CHECK(ch.fens.empty());
}

TEST_CASE("load_challenge: simple single-FEN file with file-level type/side") {
    ScopedTempFile f(
        "type: mate_in_2\n"
        "side: white\n"
        "8/8/8/8/8/8/8/4K3 w - - 0 1\n"
    );
    Challenge ch = load_challenge(f.path);
    REQUIRE(ch.fens.size() == 1);
    CHECK(ch.fens[0] == "8/8/8/8/8/8/8/4K3 w - - 0 1");
    CHECK(ch.type == "mate_in_2");
    CHECK(ch.starts_white);
    CHECK(ch.max_moves == 2);
}

TEST_CASE("load_challenge: side=black is parsed for the active puzzle") {
    ScopedTempFile f(
        "type: mate_in_1\n"
        "side: black\n"
        "4k3/8/8/8/8/8/8/4K3 b - - 0 1\n"
    );
    Challenge ch = load_challenge(f.path);
    REQUIRE(ch.fens.size() == 1);
    CHECK_FALSE(ch.starts_white);
    CHECK(ch.max_moves == 1);
}

TEST_CASE("load_challenge: per-page type and side override file defaults") {
    // File default: mate_in_2 / white. Page 2 overrides to find_forks
    // / black, and the loader must surface that for the puzzle on page 2.
    ScopedTempFile f(
        "type: mate_in_2\n"
        "side: white\n"
        "# Page 1\n"
        "8/8/8/8/8/8/8/4K3 w - - 0 1\n"
        "# Page 2\n"
        "type: find_forks\n"
        "side: black\n"
        "4k3/8/8/8/8/8/8/4K3 b - - 0 1\n"
    );
    Challenge ch = load_challenge(f.path);
    REQUIRE(ch.fens.size() == 2);
    REQUIRE(ch.fen_types.size() == 2);
    REQUIRE(ch.fen_starts_white.size() == 2);

    CHECK(ch.fen_types[0] == "mate_in_2");
    CHECK(ch.fen_starts_white[0] == true);
    CHECK(ch.fen_types[1] == "find_forks");
    CHECK(ch.fen_starts_white[1] == false);

    // current_index defaults to 0 → fields reflect page 1.
    CHECK(ch.type == "mate_in_2");
    CHECK(ch.starts_white);
    CHECK(ch.max_moves == 2);
}

TEST_CASE("load_challenge: missing top-level type defaults to mate_in_2/white") {
    ScopedTempFile f("4k3/8/8/8/8/8/8/4K3 w - - 0 1\n");
    Challenge ch = load_challenge(f.path);
    REQUIRE(ch.fens.size() == 1);
    CHECK(ch.fen_types[0] == "mate_in_2");
    CHECK(ch.fen_starts_white[0] == true);
}

TEST_CASE("load_challenge: name field overrides default filename-based name") {
    ScopedTempFile f("name: My Custom Puzzle\n4k3/8/8/8/8/8/8/4K3 w - - 0 1\n");
    Challenge ch = load_challenge(f.path);
    CHECK(ch.name == "My Custom Puzzle");
}

// ===========================================================================
// challenge_apply_current
// ===========================================================================
TEST_CASE("challenge_apply_current: switches type/starts_white/max_moves") {
    ScopedTempFile f(
        "type: mate_in_2\n"
        "side: white\n"
        "# Page 1\n"
        "8/8/8/8/8/8/8/4K3 w - - 0 1\n"
        "# Page 2\n"
        "type: find_forks\n"
        "side: black\n"
        "4k3/8/8/8/8/8/8/4K3 b - - 0 1\n"
    );
    Challenge ch = load_challenge(f.path);
    REQUIRE(ch.fens.size() == 2);

    challenge_apply_current(ch, 1);
    CHECK(ch.current_index == 1);
    CHECK(ch.type == "find_forks");
    CHECK_FALSE(ch.starts_white);
    CHECK(ch.max_moves == 1);  // find_forks → 1
    CHECK(ch.required_moves.empty());
    CHECK(ch.found_moves.empty());
}

TEST_CASE("challenge_apply_current: out-of-range index is a no-op") {
    ScopedTempFile f(
        "type: mate_in_2\nside: white\n4k3/8/8/8/8/8/8/4K3 w - - 0 1\n");
    Challenge ch = load_challenge(f.path);
    int prev_index = ch.current_index;
    std::string prev_type = ch.type;

    challenge_apply_current(ch, 99);
    CHECK(ch.current_index == prev_index);
    CHECK(ch.type == prev_type);

    challenge_apply_current(ch, -1);
    CHECK(ch.current_index == prev_index);
    CHECK(ch.type == prev_type);
}

TEST_CASE("challenge_apply_current: clears stale found/required move lists") {
    ScopedTempFile f(
        "type: mate_in_2\nside: white\n4k3/8/8/8/8/8/8/4K3 w - - 0 1\n");
    Challenge ch = load_challenge(f.path);
    ch.required_moves = {"a1a2", "b1b2"};
    ch.found_moves = {"a1a2"};
    challenge_apply_current(ch, 0);
    CHECK(ch.required_moves.empty());
    CHECK(ch.found_moves.empty());
}

// ===========================================================================
// list_challenge_files
// ===========================================================================
TEST_CASE("list_challenge_files: returns sorted .md paths") {
    ScopedTempDir d;
    d.write_file("zebra.md",   "4k3/8/8/8/8/8/8/4K3 w - - 0 1\n");
    d.write_file("alpha.md",   "4k3/8/8/8/8/8/8/4K3 w - - 0 1\n");
    d.write_file("middle.md",  "4k3/8/8/8/8/8/8/4K3 w - - 0 1\n");
    d.write_file("ignore.txt", "not a challenge\n");

    auto files = list_challenge_files(d.path);
    REQUIRE(files.size() == 3);
    // Sorted alphabetically.
    CHECK(files[0].find("alpha.md")  != std::string::npos);
    CHECK(files[1].find("middle.md") != std::string::npos);
    CHECK(files[2].find("zebra.md")  != std::string::npos);
}

TEST_CASE("list_challenge_files: nonexistent directory returns empty") {
    auto files = list_challenge_files("/tmp/3dchess_definitely_no_dir_xyz");
    CHECK(files.empty());
}
