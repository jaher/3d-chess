#pragma once

#include <string>

// Chess.com Daily / Random Puzzle integration.
//
// The publicly-available API at api.chess.com/pub/puzzle and
// api.chess.com/pub/puzzle/random returns a small JSON envelope with
// the puzzle's starting FEN, title, and a PGN body containing the
// expected solution moves. We only consume `fen` + `title` here; the
// solve flow validates user moves against Stockfish's best-move at
// each ply, which avoids having to write a full SAN parser and works
// well in practice (chess.com puzzles are tactical positions where
// the engine's choice agrees with the puzzle's expected line).
struct Puzzle {
    std::string title;
    std::string url;
    std::string fen;     // starting position
};

// Pull a top-level string field's value out of a JSON body. Handles
// the small set of escape sequences chess.com uses in titles
// (\\", \\\\, \\/, \\n) — not a full RFC-8259 parser; just enough for
// the puzzle envelope. Returns false if the key isn't found or the
// quoted value is malformed.
bool puzzle_parse_string_field(const std::string& body,
                               const std::string& key,
                               std::string& out);

// Parse a chess.com puzzle JSON envelope into the struct. Returns
// false if the FEN field is missing/empty (other fields are
// best-effort). The caller is expected to have fetched the body
// from /pub/puzzle or /pub/puzzle/random.
bool puzzle_parse_json(const std::string& body, Puzzle& out);
