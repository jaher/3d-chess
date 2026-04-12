#include "challenge.h"
#include "chess_rules.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

#include <dirent.h>

// ---------------------------------------------------------------------------
// Directory listing
// ---------------------------------------------------------------------------
std::vector<std::string> list_challenge_files(const std::string& dir) {
    std::vector<std::string> files;
    DIR* d = opendir(dir.c_str());
    if (!d) return files;
    while (struct dirent* entry = readdir(d)) {
        std::string name = entry->d_name;
        if (name.size() > 3 && name.substr(name.size() - 3) == ".md")
            files.push_back(dir + "/" + name);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    return files;
}

// ---------------------------------------------------------------------------
// Trim whitespace
// ---------------------------------------------------------------------------
static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

// ---------------------------------------------------------------------------
// Parse a challenge .md file
// ---------------------------------------------------------------------------
Challenge load_challenge(const std::string& path) {
    Challenge ch;
    ch.path = path;

    // Extract filename without extension as default name
    size_t slash = path.find_last_of('/');
    std::string fname = (slash != std::string::npos) ? path.substr(slash + 1) : path;
    size_t dot = fname.find_last_of('.');
    ch.name = (dot != std::string::npos) ? fname.substr(0, dot) : fname;

    std::ifstream file(path);
    if (!file.is_open()) return ch;

    std::string line;
    while (std::getline(file, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;

        // Try to parse as key:value
        size_t colon = t.find(':');
        if (colon != std::string::npos && colon < 20) {
            // Check that what's before the colon looks like a key (no spaces, no slashes)
            std::string key = trim(t.substr(0, colon));
            std::string value = trim(t.substr(colon + 1));
            bool is_key = !key.empty() && key.find(' ') == std::string::npos &&
                          key.find('/') == std::string::npos;
            if (is_key) {
                if (key == "type") {
                    ch.type = value;
                    if (value == "mate_in_2") ch.max_moves = 2;
                    else if (value == "mate_in_1") ch.max_moves = 1;
                    else if (value == "mate_in_3") ch.max_moves = 3;
                } else if (key == "side") {
                    ch.starts_white = (value == "white" || value == "w");
                } else if (key == "name") {
                    ch.name = value;
                }
                continue;
            }
        }

        // Otherwise treat as a FEN line
        ch.fens.push_back(t);
    }
    return ch;
}

// ---------------------------------------------------------------------------
// FEN parser
// ---------------------------------------------------------------------------
ParsedFEN parse_fen(const std::string& fen) {
    ParsedFEN result;

    // Split into fields
    std::istringstream iss(fen);
    std::string position, side, castling, en_passant, halfmove, fullmove;
    iss >> position >> side >> castling >> en_passant >> halfmove >> fullmove;

    if (position.empty()) return result;

    // Parse piece placement (rank 8 first, files a-h)
    int row = 7;
    int file_idx = 0; // 0 = a-file
    for (char c : position) {
        if (c == '/') {
            row--;
            file_idx = 0;
        } else if (c >= '1' && c <= '8') {
            file_idx += c - '0';
        } else {
            // Internal col: a-file (file_idx 0) → col 7, h-file (7) → col 0
            int col = 7 - file_idx;
            if (row < 0 || row > 7 || col < 0 || col > 7) continue;

            bool is_white = std::isupper(static_cast<unsigned char>(c)) != 0;
            char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            PieceType type;
            switch (lc) {
                case 'k': type = KING; break;
                case 'q': type = QUEEN; break;
                case 'r': type = ROOK; break;
                case 'b': type = BISHOP; break;
                case 'n': type = KNIGHT; break;
                case 'p': type = PAWN; break;
                default: file_idx++; continue;
            }
            BoardPiece p;
            p.type = type;
            p.is_white = is_white;
            p.col = col;
            p.row = row;
            p.alive = true;
            result.pieces.push_back(p);
            file_idx++;
        }
    }

    // Side to move
    result.white_turn = side.empty() || side == "w";

    // Castling rights — flag is "moved" if rights are NOT present
    result.castling.white_king_moved = castling.find('K') == std::string::npos &&
                                       castling.find('Q') == std::string::npos;
    result.castling.black_king_moved = castling.find('k') == std::string::npos &&
                                       castling.find('q') == std::string::npos;
    result.castling.white_rook_h_moved = castling.find('K') == std::string::npos;
    result.castling.white_rook_a_moved = castling.find('Q') == std::string::npos;
    result.castling.black_rook_h_moved = castling.find('k') == std::string::npos;
    result.castling.black_rook_a_moved = castling.find('q') == std::string::npos;

    result.valid = !result.pieces.empty();
    return result;
}

// ---------------------------------------------------------------------------
// Apply FEN to game state
// ---------------------------------------------------------------------------
void apply_fen_to_state(GameState& gs, const ParsedFEN& parsed) {
    gs.pieces = parsed.pieces;
    gs.white_turn = parsed.white_turn;
    gs.castling = parsed.castling;
    gs.selected_col = -1;
    gs.selected_row = -1;
    gs.valid_moves.clear();
    gs.game_over = false;
    gs.game_result.clear();
    gs.move_history.clear();
    gs.score_history.clear();
    gs.snapshots.clear();
    gs.analysis_mode = false;
    gs.analysis_index = 0;
    gs.rebuild_grid();
    gs.score_history.push_back(evaluate_position(gs));
    gs.take_snapshot();
}
