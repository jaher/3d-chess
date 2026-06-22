#include "openings.h"

#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace {

// Path to the bundled TSV. On desktop we expect the binary to run
// from the repo root (or any cwd that contains the openings/
// directory); on the web build the file lands inside the Emscripten
// VFS at /openings/openings.tsv via --preload-file.
#ifdef __EMSCRIPTEN__
constexpr const char* kPath = "/openings/openings.tsv";
#else
constexpr const char* kPath = "openings/openings.tsv";
#endif

std::unordered_map<std::string, std::string> g_openings;
bool g_loaded = false;
std::mutex g_load_mu;

// Pull off the first four FEN fields; openings are keyed only by
// the static position so transpositions match.
std::string trim_fen(const std::string& fen) {
    int field = 0;
    size_t end = 0;
    while (end < fen.size() && field < 4) {
        size_t sp = fen.find(' ', end);
        if (sp == std::string::npos) { end = fen.size(); break; }
        end = sp + 1;
        ++field;
    }
    if (field < 4) return fen;          // ran out — return unchanged
    if (end > 0 && fen[end - 1] == ' ') --end;   // drop trailing space
    return fen.substr(0, end);
}

void load_locked() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream in(kPath);
    if (!in.is_open()) {
        std::fprintf(stderr,
            "[openings] failed to open %s — opening detection disabled\n",
            kPath);
        return;
    }
    std::string line;
    bool first = true;
    int rows = 0;
    while (std::getline(in, line)) {
        if (first) { first = false; continue; }   // skip header
        if (line.empty()) continue;
        // <fen>\t<eco>\t<name>
        size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        std::string fen  = line.substr(0, t1);
        std::string name = line.substr(t2 + 1);
        if (fen.empty() || name.empty()) continue;
        g_openings.emplace(std::move(fen), std::move(name));
        ++rows;
    }
    std::fprintf(stderr, "[openings] loaded %d entries from %s\n",
                 rows, kPath);
}

}  // namespace

std::string opening_name_for_position(const std::string& fen) {
    {
        std::lock_guard<std::mutex> lk(g_load_mu);
        load_locked();
    }
    std::string key = trim_fen(fen);
    auto it = g_openings.find(key);
    if (it == g_openings.end()) return "";
    return it->second;
}
