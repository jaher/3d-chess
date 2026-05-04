#include "puzzle.h"

#include "ai_player.h"     // move_to_uci, parse_uci_move
#include "challenge.h"     // parse_fen, apply_fen_to_state, ParsedFEN
#include "chess_rules.h"   // generate_legal_moves, uci_to_algebraic, execute_move
#include "chess_types.h"

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

namespace {

// Skip whitespace forward from `i`. Returns the new index.
size_t skip_ws(const std::string& s, size_t i) {
    while (i < s.size() &&
           (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
        ++i;
    }
    return i;
}

}  // namespace

bool puzzle_parse_string_field(const std::string& body,
                               const std::string& key,
                               std::string& out) {
    out.clear();
    // Look for a quoted "key" then the next colon, then a quoted
    // value. Naïve but adequate for the chess.com puzzle envelope,
    // which is a flat JSON object with no nested duplicates of the
    // top-level field names we care about.
    std::string needle = "\"" + key + "\"";
    size_t k = body.find(needle);
    if (k == std::string::npos) return false;
    size_t i = skip_ws(body, k + needle.size());
    if (i >= body.size() || body[i] != ':') return false;
    i = skip_ws(body, i + 1);
    if (i >= body.size() || body[i] != '"') return false;
    ++i;
    std::string acc;
    while (i < body.size()) {
        char c = body[i];
        if (c == '\\' && i + 1 < body.size()) {
            char e = body[i + 1];
            switch (e) {
            case '"':  acc.push_back('"');  break;
            case '\\': acc.push_back('\\'); break;
            case '/':  acc.push_back('/');  break;
            case 'n':  acc.push_back('\n'); break;
            case 't':  acc.push_back('\t'); break;
            case 'r':  acc.push_back('\r'); break;
            default:
                // Unknown escape — keep both chars verbatim. Good
                // enough for titles; the puzzle FEN never contains
                // backslashes.
                acc.push_back('\\');
                acc.push_back(e);
                break;
            }
            i += 2;
        } else if (c == '"') {
            out.swap(acc);
            return true;
        } else {
            acc.push_back(c);
            ++i;
        }
    }
    return false;
}

bool puzzle_parse_json(const std::string& body, Puzzle& out) {
    out = Puzzle{};
    if (!puzzle_parse_string_field(body, "fen", out.fen) || out.fen.empty()) {
        return false;
    }
    puzzle_parse_string_field(body, "title", out.title);
    puzzle_parse_string_field(body, "url",   out.url);
    puzzle_parse_string_field(body, "pgn",   out.pgn);
    return true;
}

// ---------------------------------------------------------------------------
// PGN solution parser
// ---------------------------------------------------------------------------
namespace {

// Strip suffixes that uci_to_algebraic doesn't emit (annotation
// chars, NAG markers) and unify "0-0" with "O-O" so the output of
// uci_to_algebraic matches the input SAN by literal-string equality.
std::string normalize_san(std::string s) {
    for (auto& c : s) {
        if (c == '0') c = 'O';   // some PGNs spell castling with zeros
    }
    while (!s.empty()) {
        char c = s.back();
        if (c == '!' || c == '?') s.pop_back();
        else break;
    }
    return s;
}

bool is_pgn_skippable(const std::string& tok) {
    if (tok.empty()) return true;
    // Result tokens.
    if (tok == "1-0" || tok == "0-1" || tok == "1/2-1/2" || tok == "*") return true;
    // Move-number tokens: "1.", "1...", "12." — all-digit with at
    // least one '.', no SAN content.
    bool has_digit = false;
    for (char c : tok) {
        if (std::isdigit(static_cast<unsigned char>(c))) { has_digit = true; continue; }
        if (c == '.') continue;
        return false;
    }
    return has_digit;
}

// Tokenise a PGN body into SAN move strings, dropping headers
// `[Tag "value"]`, comments `{...}`, recursive variations `(...)`,
// NAGs `$N`, move numbers, and the result token.
std::vector<std::string> tokenize_pgn(const std::string& pgn) {
    std::vector<std::string> raw;
    std::string buf;
    int paren = 0, brace = 0;
    bool in_header = false;
    auto flush = [&]() {
        if (!buf.empty()) {
            raw.push_back(buf);
            buf.clear();
        }
    };
    for (size_t i = 0; i < pgn.size(); ++i) {
        char c = pgn[i];
        if (in_header) {
            if (c == ']') in_header = false;
            continue;
        }
        // PGN headers always sit at the start of a line.
        if (c == '[' && (i == 0 || pgn[i - 1] == '\n')) {
            flush();
            in_header = true;
            continue;
        }
        if (c == '{') { flush(); ++brace; continue; }
        if (c == '}') { if (brace) --brace; continue; }
        if (brace > 0) continue;
        if (c == '(') { flush(); ++paren; continue; }
        if (c == ')') { if (paren) --paren; continue; }
        if (paren > 0) continue;
        if (c == '$') {
            flush();
            // Skip until next whitespace.
            while (i + 1 < pgn.size() &&
                   !std::isspace(static_cast<unsigned char>(pgn[i + 1]))) {
                ++i;
            }
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            flush();
            continue;
        }
        buf.push_back(c);
    }
    flush();
    std::vector<std::string> out;
    out.reserve(raw.size());
    for (auto& t : raw) {
        if (!is_pgn_skippable(t)) out.push_back(normalize_san(t));
    }
    return out;
}

// Find the legal move at the current position whose SAN matches
// `target_san`. Returns the move's UCI on hit, empty string on
// miss / ambiguous match.
std::string san_to_uci_brute(GameState& gs, const std::string& target_san) {
    BoardSnapshot snap;
    snap.pieces        = gs.pieces;
    snap.white_turn    = gs.white_turn;
    snap.castling      = gs.castling;
    snap.ep_target_col = gs.ep_target_col;
    snap.ep_target_row = gs.ep_target_row;

    std::string match;
    int hits = 0;
    for (const auto& p : gs.pieces) {
        if (!p.alive) continue;
        if (p.is_white != gs.white_turn) continue;
        auto moves = generate_legal_moves(gs, p.col, p.row);
        for (const auto& [tc, tr] : moves) {
            std::string uci = move_to_uci(p.col, p.row, tc, tr);
            std::string san = normalize_san(uci_to_algebraic(snap, uci));
            if (san == target_san) {
                match = uci;
                ++hits;
            }
        }
    }
    return hits == 1 ? match : std::string();
}

}  // namespace

std::vector<std::string> puzzle_parse_solution_uci(const std::string& fen,
                                                   const std::string& pgn) {
    std::vector<std::string> out;
    if (pgn.empty()) return out;
    ParsedFEN parsed = parse_fen(fen);
    if (!parsed.valid) return out;

    GameState gs;
    apply_fen_to_state(gs, parsed);

    auto sans = tokenize_pgn(pgn);
    out.reserve(sans.size());
    for (const auto& san : sans) {
        std::string uci = san_to_uci_brute(gs, san);
        if (uci.empty()) {
            // Couldn't pin the SAN down to a single legal move — bail
            // so the caller falls back to Stockfish from the start
            // rather than playing a half-line.
            return {};
        }
        int fc, fr, tc, tr;
        if (!parse_uci_move(uci, fc, fr, tc, tr)) return {};
        execute_move(gs, fc, fr, tc, tr);
        out.push_back(std::move(uci));
    }
    return out;
}

#ifndef __EMSCRIPTEN__
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

namespace {

// Returns "YYYY-MM-DD" for the current local date — fallback name
// when a puzzle has no title.
std::string today_yyyy_mm_dd() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

// Convert an arbitrary puzzle title into a safe POSIX filename.
// Anything outside [A-Za-z0-9._-] becomes '_', runs collapse, and
// leading/trailing '_' are stripped. Returns the empty string if
// the title sanitises away to nothing — caller picks a fallback.
std::string sanitize_title_to_filename(const std::string& title) {
    std::string out;
    out.reserve(title.size());
    bool prev_under = false;
    for (char c : title) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  c == '.' || c == '-' || c == '_';
        if (ok) {
            out += c;
            prev_under = false;
        } else if (!out.empty() && !prev_under) {
            out += '_';
            prev_under = true;
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    constexpr size_t kMaxLen = 80;
    if (out.size() > kMaxLen) {
        out.resize(kMaxLen);
        while (!out.empty() && out.back() == '_') out.pop_back();
    }
    return out;
}

bool ensure_dir(const char* path) {
    struct stat st{};
    if (stat(path, &st) == 0) return (st.st_mode & S_IFDIR) != 0;
    return mkdir(path, 0775) == 0;
}

bool file_exists(const char* path) {
    struct stat st{};
    return stat(path, &st) == 0 && (st.st_mode & S_IFREG) != 0;
}

// True if any *.md directly inside puzzles/ contains `fen` as a
// standalone line. The in-app archiver writes the FEN on its own
// line so the literal-string match is reliable.
bool fen_already_archived(const std::string& fen) {
    DIR* d = opendir("puzzles");
    if (!d) return false;
    bool found = false;
    while (struct dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        if (name.size() < 3 || name.substr(name.size() - 3) != ".md") continue;
        std::string path = std::string("puzzles/") + name;
        std::ifstream f(path);
        if (!f.is_open()) continue;
        std::string line;
        while (std::getline(f, line)) {
            // Strip a trailing '\r' so files saved with CRLF line
            // endings still match.
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line == fen) { found = true; break; }
        }
        if (found) break;
    }
    closedir(d);
    return found;
}

// Indent every line of `body` with "# " so it becomes a comment
// block in the saved .md file. Empty lines stay empty (no trailing
// space).
std::string indent_as_comment(const std::string& body) {
    std::string out;
    out.reserve(body.size() + 32);
    bool at_line_start = true;
    for (char c : body) {
        if (at_line_start) {
            if (c != '\n') out += "# ";
            at_line_start = false;
        }
        out += c;
        if (c == '\n') at_line_start = true;
    }
    return out;
}

std::string fen_side(const std::string& fen) {
    size_t sp = fen.find(' ');
    if (sp == std::string::npos || sp + 1 >= fen.size()) return "white";
    return fen[sp + 1] == 'b' ? "black" : "white";
}

}  // namespace

bool puzzle_archive_save(const Puzzle& p) {
    if (p.fen.empty()) return false;
    if (!ensure_dir("puzzles")) {
        std::fprintf(stderr,
            "[puzzle] couldn't create ./puzzles/ — archive skipped\n");
        return false;
    }

    // Dedup against every .md already in puzzles/. A daily that
    // chess.com reposts later, or that the cron's random fetch
    // surfaces by chance, doesn't get a second copy.
    if (fen_already_archived(p.fen)) {
        return true;  // benign — we already have this puzzle.
    }

    // Filename = `<sanitised-title>_<today>.md`. If the title is
    // empty (chess.com sometimes ships untitled random puzzles)
    // we fall back to `<today>.md`. Same-title-same-day collisions
    // (different FEN) get a numeric suffix so neither file is
    // clobbered.
    std::string today = today_yyyy_mm_dd();
    std::string title_slug = sanitize_title_to_filename(p.title);
    std::string base = title_slug.empty()
        ? today
        : (title_slug + "_" + today);
    std::string path = "puzzles/" + base + ".md";
    int suffix = 2;
    while (file_exists(path.c_str())) {
        path = "puzzles/" + base + "_" + std::to_string(suffix) + ".md";
        ++suffix;
    }

    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        std::fprintf(stderr, "[puzzle] open %s for write failed\n",
                     path.c_str());
        return false;
    }
    std::fprintf(f,
        "# Chess.com Daily Puzzle archive\n"
        "#\n"
        "# Fetched on %s.\n",
        today.c_str());
    if (!p.title.empty()) std::fprintf(f, "# title: %s\n", p.title.c_str());
    if (!p.url.empty())   std::fprintf(f, "# url:   %s\n", p.url.c_str());
    std::fprintf(f, "\n");

    if (!p.title.empty()) {
        std::fprintf(f, "name: %s\n\n", p.title.c_str());
    }
    std::fprintf(f, "type: puzzle\n");
    std::fprintf(f, "side: %s\n", fen_side(p.fen).c_str());
    std::fprintf(f, "%s\n", p.fen.c_str());

    if (!p.pgn.empty()) {
        std::fprintf(f, "\n# --- Solution PGN (chess.com) ---\n");
        std::string commented = indent_as_comment(p.pgn);
        std::fputs(commented.c_str(), f);
        if (!commented.empty() && commented.back() != '\n') {
            std::fputc('\n', f);
        }
    }
    std::fclose(f);
    std::fprintf(stderr, "[puzzle] archived %s\n", path.c_str());
    return true;
}

#else  // __EMSCRIPTEN__

bool puzzle_archive_save(const Puzzle& /*p*/) {
    // Web build runs in a browser sandbox with no writable working
    // directory, so we just no-op. The shared layer's caller does
    // not branch on this — it just gets `false` back.
    return false;
}

#endif
