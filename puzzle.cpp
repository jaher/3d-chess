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
    return true;
}
