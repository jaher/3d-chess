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

#include <vector>

// Convert the SAN move list embedded in a chess.com puzzle PGN
// body into a sequence of UCI strings, given the puzzle's
// starting FEN. The implementation is brute-force-by-comparison:
// for each SAN token, we generate every legal move at the
// current position, compute its SAN via uci_to_algebraic, and
// match. This avoids having to write a full SAN parser at the
// cost of an O(legal-moves) loop per ply — fine since chess.com
// solution lines are typically <10 plies.
//
// Returns an empty vector if any of the following hold:
//   * pgn is empty / contains no SAN tokens after stripping
//     headers, comments, NAGs, variations, the result token and
//     move numbers;
//   * fen fails to parse;
//   * any SAN token has no matching legal move at its turn — we
//     bail rather than half-fill the list, since downstream
//     consumers want "trust the line or don't trust anything."
//
// The puzzle play flow uses this to drive Stockfish-free move
// playback while the user follows the canonical line; the moment
// the user diverges from it, the caller falls back to a regular
// Stockfish bestmove for the AI's reply.
std::vector<std::string> puzzle_parse_solution_uci(const std::string& fen,
                                                   const std::string& pgn);

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
