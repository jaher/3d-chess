#pragma once

#include <string>
#include <vector>

// Opening-line drills for the trainer. Each drill is a named main line
// the human plays out (their side moves first) while the app auto-plays
// the book replies — built on the existing puzzle solution-follower.

struct OpeningDrill {
    std::string name;                 // e.g. "Italian Game"
    std::vector<std::string> moves;   // UCI moves in order, side-to-move first
};

// Parse the drills file body: blocks of "name: …" then "line: <uci…>",
// '#' comments and blank lines ignored. Blocks missing a name or any
// moves are skipped. Pure (no I/O) so it is unit-testable.
std::vector<OpeningDrill> parse_opening_drills(const std::string& text);

// Read + parse the drills file; empty vector on read failure.
std::vector<OpeningDrill> load_opening_drills(const std::string& path);
