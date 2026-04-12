#pragma once

#include "chess_types.h"
#include <string>
#include <vector>

struct Challenge {
    std::string name;          // filename without extension
    std::string path;          // full file path
    std::string type;          // e.g. "mate_in_2"
    bool starts_white = true;  // which side moves first
    int max_moves = 0;         // max moves for the starting side (0 = unlimited)
    std::vector<std::string> fens;
    int current_index = 0;     // currently active puzzle within fens
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

// Convert a FEN string into a board state (uses internal column mapping)
ParsedFEN parse_fen(const std::string& fen);

// Apply a parsed FEN to the GameState (resets selection, snapshots, etc.)
void apply_fen_to_state(GameState& gs, const ParsedFEN& parsed);
