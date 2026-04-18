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
// Type → max_moves table
// ---------------------------------------------------------------------------
static int max_moves_for_type(const std::string& t) {
    if (t == "mate_in_1")  return 1;
    if (t == "mate_in_2")  return 2;
    if (t == "mate_in_3")  return 3;
    if (t == "find_forks") return 1;
    if (t == "find_pins")  return 1;
    return 0;
}

// Match a comment like "Page 1", "Page  3" — case-insensitive.
static bool is_page_marker(const std::string& comment) {
    if (comment.size() < 5) return false;
    auto starts_with_ci = [](const std::string& s, const char* prefix) {
        size_t n = std::strlen(prefix);
        if (s.size() < n) return false;
        for (size_t i = 0; i < n; ++i) {
            if (std::tolower(static_cast<unsigned char>(s[i])) !=
                std::tolower(static_cast<unsigned char>(prefix[i])))
                return false;
        }
        return true;
    };
    if (!starts_with_ci(comment, "page")) return false;
    size_t i = 4;
    if (i >= comment.size() ||
        !std::isspace(static_cast<unsigned char>(comment[i]))) return false;
    while (i < comment.size() &&
           std::isspace(static_cast<unsigned char>(comment[i]))) ++i;
    bool any = false;
    while (i < comment.size() &&
           std::isdigit(static_cast<unsigned char>(comment[i]))) {
        any = true; ++i;
    }
    return any;
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

    // File-level defaults (set by top-level type:/side: lines before
    // the first "# Page N" marker). Per-page lines override.
    std::string file_type;
    std::string file_side;
    std::string current_type;
    std::string current_side;
    int pages_seen = 0;

    std::string line;
    while (std::getline(file, line)) {
        std::string t = trim(line);
        if (t.empty()) continue;

        if (t[0] == '#') {
            std::string comment = trim(t.substr(1));
            if (is_page_marker(comment)) {
                // Reset per-page state back to file defaults before
                // the page's own metadata arrives.
                current_type = file_type;
                current_side = file_side;
                ++pages_seen;
            }
            continue;
        }

        size_t colon = t.find(':');
        if (colon != std::string::npos && colon < 20) {
            std::string key = trim(t.substr(0, colon));
            std::string value = trim(t.substr(colon + 1));
            bool is_key = !key.empty() && key.find(' ') == std::string::npos &&
                          key.find('/') == std::string::npos;
            if (is_key) {
                if (key == "type") {
                    if (pages_seen == 0) {
                        file_type = value;
                        current_type = value;
                    } else {
                        current_type = value;
                    }
                    continue;
                }
                if (key == "side") {
                    if (pages_seen == 0) {
                        file_side = value;
                        current_side = value;
                    } else {
                        current_side = value;
                    }
                    continue;
                }
                if (key == "name") {
                    ch.name = value;
                    continue;
                }
            }
        }

        // FEN line — stamp it with the active per-page type/side.
        const std::string& resolved_type =
            !current_type.empty() ? current_type
            : (!file_type.empty() ? file_type : std::string("mate_in_2"));
        const std::string& resolved_side =
            !current_side.empty() ? current_side
            : (!file_side.empty() ? file_side : std::string("white"));
        ch.fens.push_back(t);
        ch.fen_types.push_back(resolved_type);
        ch.fen_starts_white.push_back(
            resolved_side == "white" || resolved_side == "w"
        );
    }

    // Default current-puzzle pointer to the first FEN.
    if (!ch.fens.empty()) {
        challenge_apply_current(ch, 0);
    } else {
        // Preserve the old single-type semantics for empty files so
        // callers see ``ch.type`` reflect the file-level default.
        ch.type = !file_type.empty() ? file_type : std::string();
        ch.starts_white = (file_side.empty() || file_side == "white" ||
                           file_side == "w");
        ch.max_moves = max_moves_for_type(ch.type);
    }
    return ch;
}

void challenge_apply_current(Challenge& ch, int index) {
    if (index < 0 || index >= static_cast<int>(ch.fens.size())) return;
    ch.current_index = index;
    if (index < static_cast<int>(ch.fen_types.size())) {
        ch.type = ch.fen_types[index];
    }
    if (index < static_cast<int>(ch.fen_starts_white.size())) {
        ch.starts_white = ch.fen_starts_white[index];
    }
    ch.max_moves = max_moves_for_type(ch.type);
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

// ---------------------------------------------------------------------------
// Tactic detection: forks & pins
// ---------------------------------------------------------------------------
static int piece_value_pts(PieceType t) {
    switch (t) {
    case PAWN:   return 1;
    case KNIGHT: return 3;
    case BISHOP: return 3;
    case ROOK:   return 5;
    case QUEEN:  return 9;
    case KING:   return 100;  // absolute pins always count
    default:     return 0;
    }
}

bool move_is_fork(const GameState& gs, int to_col, int to_row) {
    if (to_col < 0 || to_col >= 8 || to_row < 0 || to_row >= 8) return false;
    int idx = gs.grid[to_row][to_col];
    if (idx < 0) return false;
    bool attacker_white = gs.pieces[idx].is_white;

    // generate_moves is color-aware via the piece itself, so this
    // works even though gs.white_turn has flipped after the move.
    auto moves = generate_moves(gs, to_col, to_row);
    int victims = 0;
    for (const auto& m : moves) {
        int vi = gs.grid[m.second][m.first];
        if (vi >= 0 && gs.pieces[vi].is_white != attacker_white)
            ++victims;
    }
    return victims >= 2;
}

bool move_is_pin(const GameState& gs, int to_col, int to_row) {
    if (to_col < 0 || to_col >= 8 || to_row < 0 || to_row >= 8) return false;
    int idx = gs.grid[to_row][to_col];
    if (idx < 0) return false;
    const BoardPiece& attacker = gs.pieces[idx];
    if (attacker.type != BISHOP && attacker.type != ROOK &&
        attacker.type != QUEEN) return false;

    struct Dir { int dc, dr; };
    std::vector<Dir> dirs;
    if (attacker.type == BISHOP || attacker.type == QUEEN) {
        dirs.push_back({ 1,  1}); dirs.push_back({ 1, -1});
        dirs.push_back({-1,  1}); dirs.push_back({-1, -1});
    }
    if (attacker.type == ROOK || attacker.type == QUEEN) {
        dirs.push_back({0,  1}); dirs.push_back({0, -1});
        dirs.push_back({1,  0}); dirs.push_back({-1, 0});
    }

    for (const auto& d : dirs) {
        int c = to_col + d.dc, r = to_row + d.dr;
        int first = -1;
        while (c >= 0 && c < 8 && r >= 0 && r < 8) {
            int i = gs.grid[r][c];
            if (i >= 0) {
                const BoardPiece& p = gs.pieces[i];
                if (p.is_white == attacker.is_white) break;  // own piece: blocked
                if (first < 0) {
                    first = i;                // record the pinned piece
                } else {
                    // Second enemy behind the first along the same line.
                    int v1 = piece_value_pts(gs.pieces[first].type);
                    int v2 = piece_value_pts(p.type);
                    if (v2 > v1) return true;
                    break;
                }
            }
            c += d.dc; r += d.dr;
        }
    }
    return false;
}
