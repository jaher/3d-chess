// Unit tests for the Phantom v0.3.0 wire-format encoder (phantom_encode.h).
//
// The protocol facts asserted here were lifted from the decompiled official
// app and verified against a real board:
//   * a move is two frames on the GAME characteristic — a side frame
//     [0x0a]+"2" then a movement frame [0x02]+"M <from><sep><to> <piece>";
//   * a detected physical move arrives as [0x06]+"<from><sep><to> …" ASCII.
// Coordinates in/out of the encoder are file/rank indices (0 = a-file /
// rank 1).

#include "doctest.h"

#include "../phantom_encode.h"

#include <string>
#include <vector>

namespace {
std::string ascii(const std::vector<uint8_t>& v, size_t from = 0) {
    std::string s;
    for (size_t i = from; i < v.size(); ++i) s.push_back(char(v[i]));
    return s;
}
}  // namespace

TEST_CASE("make_frame prepends the opcode byte") {
    auto f = phantom::make_frame(phantom::GameOp::Movement, "M e2-e4 P");
    REQUIRE(f.size() == 10);
    CHECK(f[0] == 0x02);
    CHECK(ascii(f, 1) == "M e2-e4 P");
}

TEST_CASE("move_text builds the algebraic move string") {
    // e2 = file 4 (e), rank 1 (rank 2); e4 = file 4, rank 3.
    CHECK(phantom::move_text(4, 1, 4, 3, /*capture=*/false, 'P') == "M e2-e4 P");
    // capture uses 'x'; black knight b8 (file 1, rank 7) takes c6 (file 2,
    // rank 5).
    CHECK(phantom::move_text(1, 7, 2, 5, /*capture=*/true, 'n') == "M b8xc6 n");
    // unknown piece falls back to 'E'.
    CHECK(phantom::move_text(0, 0, 0, 1, false, 0) == "M a1-a2 E");
}

TEST_CASE("make_side_frame is the constant [0x0a]+\"2\"") {
    auto s = phantom::make_side_frame();
    REQUIRE(s.size() == 2);
    CHECK(s[0] == 0x0a);
    CHECK(s[1] == '2');
}

TEST_CASE("make_move_frames is side then movement") {
    auto frames = phantom::make_move_frames(4, 1, 4, 3, false, 'P');
    CHECK(frames[0] == phantom::make_side_frame());
    CHECK(frames[1][0] == 0x02);
    CHECK(ascii(frames[1], 1) == "M e2-e4 P");
}

TEST_CASE("parse_detected_move decodes a 0x06 inbound frame") {
    // [0x06] + "e2-e4 .. .." — only the first token matters.
    std::string body = "e2-e4 x y";
    std::vector<uint8_t> frame;
    frame.push_back(phantom::INBOUND_MOVE_OPCODE);
    for (char c : body) frame.push_back(uint8_t(c));

    int sc, sr, dc, dr; bool cap;
    REQUIRE(phantom::parse_detected_move(frame.data(), frame.size(),
                                         sc, sr, dc, dr, cap));
    CHECK(sc == 4); CHECK(sr == 1);   // e2
    CHECK(dc == 4); CHECK(dr == 3);   // e4
    CHECK(cap == false);
}

TEST_CASE("parse_detected_move flags captures and rejects junk") {
    auto build = [](const std::string& body, uint8_t op) {
        std::vector<uint8_t> f; f.push_back(op);
        for (char c : body) f.push_back(uint8_t(c));
        return f;
    };
    int sc, sr, dc, dr; bool cap;

    // capture (separator 'x'): e4 x d5.
    auto capf = build("e4xd5", phantom::INBOUND_MOVE_OPCODE);
    REQUIRE(phantom::parse_detected_move(capf.data(), capf.size(),
                                         sc, sr, dc, dr, cap));
    CHECK(sc == 4); CHECK(sr == 3);   // e4
    CHECK(dc == 3); CHECK(dr == 4);   // d5
    CHECK(cap == true);

    // wrong opcode → rejected.
    auto wrong = build("e2-e4", 0x02);
    CHECK_FALSE(phantom::parse_detected_move(wrong.data(), wrong.size(),
                                             sc, sr, dc, dr, cap));
    // off-board file → rejected.
    auto bad = build("z2-e4", phantom::INBOUND_MOVE_OPCODE);
    CHECK_FALSE(phantom::parse_detected_move(bad.data(), bad.size(),
                                             sc, sr, dc, dr, cap));
    // too short → rejected.
    std::vector<uint8_t> tiny = {phantom::INBOUND_MOVE_OPCODE, 'e', '2'};
    CHECK_FALSE(phantom::parse_detected_move(tiny.data(), tiny.size(),
                                             sc, sr, dc, dr, cap));
}

TEST_CASE("fen_piece_at reads the board field by file/rank") {
    std::string start =
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    CHECK(phantom::fen_piece_at(start, 4, 0) == 'K');  // e1 white king
    CHECK(phantom::fen_piece_at(start, 4, 7) == 'k');  // e8 black king
    CHECK(phantom::fen_piece_at(start, 4, 1) == 'P');  // e2 white pawn
    CHECK(phantom::fen_piece_at(start, 0, 7) == 'r');  // a8 black rook
    CHECK(phantom::fen_piece_at(start, 7, 0) == 'R');  // h1 white rook
    CHECK(phantom::fen_piece_at(start, 4, 3) == 0);    // e4 empty
    CHECK(phantom::fen_piece_at(start, 4, 4) == 0);    // e5 empty
    CHECK(phantom::fen_piece_at(start, -1, 0) == 0);   // out of range
}

TEST_CASE("is_phantom_name matches the brand variants") {
    CHECK(phantom::is_phantom_name("Phantom 3579"));
    CHECK(phantom::is_phantom_name("phantom"));
    CHECK(phantom::is_phantom_name("GoChess 12"));
    CHECK_FALSE(phantom::is_phantom_name("Chessnut Move"));
    CHECK_FALSE(phantom::is_phantom_name(""));
}
