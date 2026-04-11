#pragma once

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <curl/curl.h>

// ---------------------------------------------------------------------------
// FEN generation from board state
// ---------------------------------------------------------------------------
// Piece type enum must match main: KING=0, QUEEN=1, BISHOP=2, KNIGHT=3, ROOK=4, PAWN=5
inline char piece_to_fen(int type, bool is_white) {
    const char white_chars[] = "KQBNRP";
    const char black_chars[] = "kqbnrp";
    return is_white ? white_chars[type] : black_chars[type];
}

struct BoardSquare {
    int piece_type;  // -1 = empty
    bool is_white;
};

inline std::string board_to_fen(const BoardSquare board[8][8], bool white_turn,
                               bool wk_moved = false, bool bk_moved = false,
                               bool wra_moved = false, bool wrh_moved = false,
                               bool bra_moved = false, bool brh_moved = false) {
    std::string fen;

    // Ranks 8 to 1 (row 7 to 0), files a-h (internal col 7 to 0)
    for (int row = 7; row >= 0; row--) {
        int empty = 0;
        for (int col = 7; col >= 0; col--) { // a-file (col 7) to h-file (col 0)
            if (board[row][col].piece_type < 0) {
                empty++;
            } else {
                if (empty > 0) {
                    fen += std::to_string(empty);
                    empty = 0;
                }
                fen += piece_to_fen(board[row][col].piece_type,
                                    board[row][col].is_white);
            }
        }
        if (empty > 0) fen += std::to_string(empty);
        if (row > 0) fen += '/';
    }

    fen += white_turn ? " w " : " b ";

    // Castling rights
    std::string castling;
    if (!wk_moved && !wrh_moved) castling += 'K';
    if (!wk_moved && !wra_moved) castling += 'Q';
    if (!bk_moved && !brh_moved) castling += 'k';
    if (!bk_moved && !bra_moved) castling += 'q';
    if (castling.empty()) castling = "-";
    fen += castling + " - 0 1";

    return fen;
}

// ---------------------------------------------------------------------------
// UCI move helpers
// ---------------------------------------------------------------------------
// Internal col 0 = h-file, col 7 = a-file (reversed for camera orientation)
inline int internal_col_to_file(int col) { return 7 - col; } // 0->7(h), 7->0(a)
inline int file_to_internal_col(int file) { return 7 - file; } // 0(a)->7, 7(h)->0

inline std::string square_to_uci(int col, int row) {
    int file = internal_col_to_file(col);
    return std::string(1, static_cast<char>('a' + file)) +
           std::string(1, static_cast<char>('1' + row));
}

inline std::string move_to_uci(int from_col, int from_row, int to_col, int to_row) {
    return square_to_uci(from_col, from_row) + square_to_uci(to_col, to_row);
}

inline bool parse_uci_move(const std::string& move, int& from_col, int& from_row,
                           int& to_col, int& to_row) {
    if (move.size() < 4) return false;

    from_col = file_to_internal_col(move[0] - 'a');
    from_row = move[1] - '1';
    to_col   = file_to_internal_col(move[2] - 'a');
    to_row   = move[3] - '1';

    return from_col >= 0 && from_col < 8 &&
           from_row >= 0 && from_row < 8 &&
           to_col >= 0   && to_col < 8   &&
           to_row >= 0   && to_row < 8;
}

// ---------------------------------------------------------------------------
// Curl write callback
// ---------------------------------------------------------------------------
static size_t curl_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* buf = static_cast<std::string*>(userp);
    buf->append(static_cast<char*>(contents), total);
    return total;
}

// ---------------------------------------------------------------------------
// Extract text from Anthropic API JSON response
// Minimal parser: finds first "text":"..." in the content array
// ---------------------------------------------------------------------------
inline std::string extract_response_text(const std::string& json) {
    // Look for "text":" pattern after "content"
    std::string marker = "\"text\":";
    auto pos = json.find(marker);
    if (pos == std::string::npos) return "";

    pos += marker.size();
    // Skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++; // skip opening quote

    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++; // skip escape
        }
        result += json[pos];
        pos++;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Escape a string for JSON
// ---------------------------------------------------------------------------
inline std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Send a prompt to the Anthropic API and return the text response
// ---------------------------------------------------------------------------
inline std::string call_anthropic(const std::string& prompt) {
    const char* api_key = std::getenv("ANTHROPIC_API_KEY");
    if (!api_key || strlen(api_key) == 0) {
        std::fprintf(stderr, "ANTHROPIC_API_KEY not set\n");
        return "";
    }

    std::string body =
        "{"
        "\"model\":\"claude-sonnet-4-20250514\","
        "\"max_tokens\":20,"
        "\"messages\":[{\"role\":\"user\",\"content\":\"" + prompt + "\"}]"
        "}";

    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string auth_header = "x-api-key: " + std::string(api_key);
    headers = curl_slist_append(headers, auth_header.c_str());
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::fprintf(stderr, "API call failed: %s\n", curl_easy_strerror(res));
        return "";
    }

    std::string text = extract_response_text(response);
    std::fprintf(stderr, "AI response: \"%s\"\n", text.c_str());
    return text;
}

// ---------------------------------------------------------------------------
// Extract a clean UCI move (4-5 chars) from raw AI text
// ---------------------------------------------------------------------------
inline std::string extract_uci(const std::string& text) {
    std::string clean;
    for (char c : text) {
        if ((c >= 'a' && c <= 'h') || (c >= '1' && c <= '8'))
            clean += c;
        if (clean.size() >= 4) break;
    }
    return clean;
}

// ---------------------------------------------------------------------------
// Call Anthropic API to get a move for black
// Validates the move against the board and retries up to 3 times
// board_grid: [row][col] = piece index or -1
// pieces: the piece list to validate against
// ---------------------------------------------------------------------------
inline std::string ask_ai_move(const std::string& fen,
                               const std::vector<std::string>& move_history,
                               const BoardSquare board[8][8]) {
    // Build move history string
    std::string history;
    for (size_t i = 0; i < move_history.size(); i++) {
        if (i > 0) history += " ";
        if (i % 2 == 0) history += std::to_string(i / 2 + 1) + ". ";
        history += move_history[i];
    }

    std::string base_prompt =
        "You are playing as Black in a casual chess game against a beginner. "
        "Play at a beginner-friendly level. Make reasonable but imperfect moves. "
        "Occasionally miss tactics, play slightly passive moves, or make "
        "minor positional mistakes. Do NOT play the optimal engine move every "
        "time. Play like a friendly human opponent who is slightly better "
        "than a beginner but still makes mistakes.\\n\\n"
        "Current position (FEN): " + json_escape(fen) + "\\n\\n";
    if (!history.empty())
        base_prompt += "Move history: " + json_escape(history) + "\\n\\n";
    base_prompt +=
        "Respond with ONLY your next move in UCI coordinate format "
        "(e.g., e7e5, g8f6, b8c6). No explanation, no punctuation, "
        "just the 4-character move.";

    std::string previous_attempts;

    for (int attempt = 0; attempt < 3; attempt++) {
        std::string prompt = base_prompt;
        if (!previous_attempts.empty())
            prompt += "\\n\\n" + previous_attempts;

        std::string text = call_anthropic(prompt);
        if (text.empty()) return "";

        std::string uci = extract_uci(text);

        int fc, fr, tc, tr;
        if (!parse_uci_move(uci, fc, fr, tc, tr)) {
            std::fprintf(stderr, "Attempt %d: could not parse '%s'\n",
                         attempt + 1, uci.c_str());
            previous_attempts +=
                "IMPORTANT: Your previous response \\\"" + json_escape(uci) +
                "\\\" was not a valid UCI move. "
                "Please respond with a valid 4-character UCI move for a Black piece "
                "(e.g., e7e5). The source square must contain one of your Black pieces.";
            continue;
        }

        // Validate: source must be a black piece
        if (board[fr][fc].piece_type < 0 || board[fr][fc].is_white) {
            std::fprintf(stderr, "Attempt %d: %s — no black piece at source\n",
                         attempt + 1, uci.c_str());
            previous_attempts +=
                "IMPORTANT: Your previous move \\\"" + uci +
                "\\\" is invalid because there is no Black piece on " +
                square_to_uci(fc, fr) +
                ". Please pick a square that has one of your Black pieces "
                "and respond with a valid 4-character UCI move.";
            continue;
        }

        // Validate: destination must not have own piece
        if (board[tr][tc].piece_type >= 0 && !board[tr][tc].is_white) {
            std::fprintf(stderr, "Attempt %d: %s — destination has own piece\n",
                         attempt + 1, uci.c_str());
            previous_attempts +=
                "IMPORTANT: Your previous move \\\"" + uci +
                "\\\" is invalid because " + square_to_uci(tc, tr) +
                " is occupied by your own piece. "
                "Please respond with a different valid 4-character UCI move.";
            continue;
        }

        // Move looks structurally valid
        return uci;
    }

    std::fprintf(stderr, "AI failed to produce a valid move after 3 attempts\n");
    return "";
}
