#include "puzzle.h"

#include <cctype>
#include <cstddef>
#include <string>

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

#ifndef __EMSCRIPTEN__
#include <sys/stat.h>
#include <cstdio>
#include <ctime>

namespace {

// Rewrite characters that aren't safe in a filename so the saved
// path doesn't accidentally escape the puzzles directory.
char sanitize_path_char(char c) {
    if (c == '/' || c == '\\' || c == '\0') return '_';
    return c;
}

// Returns "YYYY-MM-DD" for the current local date.
std::string today_yyyy_mm_dd() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    // 32 bytes is more than enough for "YYYY-MM-DD\0" (11 bytes)
    // even at GCC's worst-case format-truncation analysis, where
    // tm_year is treated as a full int and could in theory overflow
    // the %04d field. Using 16 trips -Wformat-truncation; 32 keeps
    // the compiler quiet without playing tricks with format specs.
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
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

// Indent every line of `body` with "# " so it becomes a comment
// block in the saved .md file. Empty lines stay empty (no trailing
// space).
std::string indent_as_comment(const std::string& body) {
    std::string out;
    out.reserve(body.size() + 32);
    bool at_line_start = true;
    for (char c : body) {
        if (at_line_start) {
            if (c != '\n') {
                out += "# ";
            }
            at_line_start = false;
        }
        out += c;
        if (c == '\n') at_line_start = true;
    }
    return out;
}

// Side-to-move parsed from the FEN's second token. Returns "white"
// / "black" / "white" (default fallback).
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
            "[puzzle] couldn't create ./puzzles/ — archive write skipped\n");
        return false;
    }

    // Filename is the local date plus a sanitised slug from the
    // chess.com URL's last path segment, so two same-day fetches
    // (daily + a manually-fetched one later) don't clobber each
    // other. The first daily of the day always wins via
    // file_exists short-circuit below.
    std::string slug;
    if (!p.url.empty()) {
        size_t cut = p.url.find_last_of('/');
        slug = (cut == std::string::npos) ? p.url : p.url.substr(cut + 1);
    }
    for (char& c : slug) c = sanitize_path_char(c);
    if (slug.size() > 60) slug.resize(60);

    std::string base = today_yyyy_mm_dd();
    std::string path = std::string("puzzles/") + base;
    if (!slug.empty()) {
        path += "_";
        path += slug;
    }
    path += ".md";

    if (file_exists(path.c_str())) {
        // Already saved earlier today (re-entry to the puzzle screen
        // shouldn't write again).
        return true;
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
        "# Fetched on %s. Format mirrors challenges/*.md so this\n"
        "# file is greppable / replayable as a single-page challenge.\n",
        base.c_str());
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
