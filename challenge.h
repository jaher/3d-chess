#pragma once

#include "chess_types.h"
#include <string>
#include <vector>

struct Challenge {
    std::string name;          // filename without extension
    std::string path;          // full file path
    // ``type``, ``starts_white`` and ``max_moves`` always reflect the
    // CURRENTLY-active puzzle (``fens[current_index]``). A multi-page
    // homework file can mix tactic types; call
    // ``challenge_apply_current`` after mutating ``current_index``
    // to sync these fields.
    std::string type;          // e.g. "mate_in_2", "find_forks"
    bool starts_white = true;  // which side moves first
    int max_moves = 0;         // max moves for the starting side (0 = unlimited)
    std::vector<std::string> fens;
    std::vector<std::string> fen_types;        // per-puzzle type
    std::vector<bool> fen_starts_white;        // per-puzzle side-to-move
    int current_index = 0;     // currently active puzzle within fens

    // Populated for ``find_forks`` / ``find_pins`` puzzles on load.
    // ``required_moves`` lists every UCI move the starter can play
    // to create the target motif; ``found_moves`` is the subset the
    // user has already played. The puzzle is solved when the two
    // sets are equal.
    std::vector<std::string> required_moves;
    std::vector<std::string> found_moves;
};

struct ParsedFEN {
    std::vector<BoardPiece> pieces;
    bool white_turn = true;
    CastlingRights castling;
    bool valid = false;
};

// Discover challenge files in a directory (returns paths sorted by name)
std::vector<std::string> list_challenge_files(const std::string& dir);

// Load and parse a single challenge file
Challenge load_challenge(const std::string& path);

// Sync the Challenge's ``type`` / ``starts_white`` / ``max_moves`` to
// the puzzle at the given index from the ``fen_types`` /
// ``fen_starts_white`` arrays. No-op for out-of-range indices.
void challenge_apply_current(Challenge& ch, int index);

// Tactic-puzzle success checks. Both take the destination square of
// the move that was just executed (the moved piece lives there now).
//
//  - move_is_fork: true iff the moved piece attacks 2+ enemy pieces.
//  - move_is_pin : true iff the moved piece (must be B/R/Q) now
//                  sits on a line between two enemy pieces with the
//                  farther one strictly more valuable (absolute or
//                  relative pin).
bool move_is_fork(const GameState& gs, int to_col, int to_row);
bool move_is_pin(const GameState& gs, int to_col, int to_row);

// Enumerate every legal move for ``white_side`` that produces the
// given motif. Returned as UCI strings in no particular order.
// ``tactic_type`` must be ``"find_forks"`` or ``"find_pins"``; any
// other value returns an empty list.
std::vector<std::string> find_tactic_moves(
    GameState& gs, bool white_side, const std::string& tactic_type);

// Convert a FEN string into a board state (uses internal column mapping)
ParsedFEN parse_fen(const std::string& fen);

// Apply a parsed FEN to the GameState (resets selection, snapshots, etc.)
void apply_fen_to_state(GameState& gs, const ParsedFEN& parsed);
