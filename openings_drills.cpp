#include "openings_drills.h"

#include <cstdio>
#include <sstream>

namespace {
// Trim leading/trailing ASCII whitespace.
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r')) --b;
    return s.substr(a, b - a);
}
}  // namespace

std::vector<OpeningDrill> parse_opening_drills(const std::string& text) {
    std::vector<OpeningDrill> out;
    OpeningDrill cur;

    auto flush = [&]() {
        if (!cur.name.empty() && !cur.moves.empty()) out.push_back(cur);
        cur = OpeningDrill{};
    };

    std::istringstream lines(text);
    std::string raw;
    while (std::getline(lines, raw)) {
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(line.substr(0, colon));
        std::string val = trim(line.substr(colon + 1));
        if (key == "name") {
            // A new name starts a new block; flush whatever came before.
            flush();
            cur.name = val;
        } else if (key == "line") {
            std::istringstream toks(val);
            std::string mv;
            while (toks >> mv) {
                if (mv.size() >= 4) cur.moves.push_back(mv);
            }
        }
    }
    flush();
    return out;
}

std::vector<OpeningDrill> load_opening_drills(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return {};
    std::string text;
    char chunk[512];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
        text.append(chunk, n);
    std::fclose(f);
    return parse_opening_drills(text);
}
