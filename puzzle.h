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
    std::string pgn;     // solution PGN body (kept verbatim for the archive)
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

// Persist a fetched puzzle to ``puzzles/YYYY-MM-DD.md`` (relative
// to the current working directory) in a layout that mirrors the
// challenges/*.md format: a `name:` header, a `# url`/`# saved`
// comment block, the FEN on its own line, and the original PGN
// body inside a comment block so the file stays parseable as a
// single-FEN challenge file. Idempotent: existing files are not
// overwritten so the same daily puzzle visited twice in a session
// only writes once. No-op on platforms without a writable working
// directory (web build returns false unconditionally).
//
// Returns true when a file was created (or already existed for the
// same date), false on directory / write failure.
bool puzzle_archive_save(const Puzzle& p);
