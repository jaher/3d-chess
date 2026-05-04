#include "voice_input.h"

#include "ai_player.h"     // move_to_uci
#include "chess_rules.h"   // generate_legal_moves

#include <cctype>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string lowercase(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Replace whole-word `from` with `to`. Word boundary = start/end of
// string or any non-alphanumeric character.
std::string replace_word(const std::string& s,
                         const std::string& from,
                         const std::string& to) {
    if (from.empty()) return s;
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        size_t pos = s.find(from, i);
        if (pos == std::string::npos) {
            out.append(s, i, std::string::npos);
            break;
        }
        bool prev_ok = (pos == 0 ||
                        !std::isalnum(static_cast<unsigned char>(s[pos - 1])));
        size_t end = pos + from.size();
        bool next_ok = (end >= s.size() ||
                        !std::isalnum(static_cast<unsigned char>(s[end])));
        if (prev_ok && next_ok) {
            out.append(s, i, pos - i);
            out.append(to);
            i = end;
        } else {
            out.append(s, i, pos - i + 1);
            i = pos + 1;
        }
    }
    return out;
}

// Lowercase, swap common Whisper homophones, drop filler verbs,
// normalise punctuation/whitespace.
std::string normalize(const std::string& utterance) {
    std::string s = lowercase(utterance);

    // Punctuation → space (so "knight, d3" works). Keep alnum and '-'.
    for (char& c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-')
            c = ' ';
    }

    // Common ASR substitutions. "right" misheard for "rook" is the
    // most common one. "ex" / "by" can also slip in for capture.
    s = replace_word(s, "night",   "knight");
    s = replace_word(s, "right",   "rook");
    s = replace_word(s, "roo",     "rook");
    s = replace_word(s, "queens",  "queen");

    // Spelled digits.
    const std::pair<const char*, const char*> digits[] = {
        {"one",   "1"}, {"two",   "2"}, {"three", "3"}, {"four",  "4"},
        {"five",  "5"}, {"six",   "6"}, {"seven", "7"}, {"eight", "8"},
    };
    for (const auto& d : digits) s = replace_word(s, d.first, d.second);

    // Filler / connective verbs. Keep this list short — single letter
    // words are NOT dropped because they may be file disambiguators
    // (e.g. "rook a takes b7").
    for (const auto& f : {"to", "from", "the", "and", "takes", "take",
                          "captures", "capture", "promote",
                          "promotion", "moves", "move"})
        s = replace_word(s, f, "");

    // 'x' as a separate word → capture marker. Strip it.
    s = replace_word(s, "x", "");

    // Collapse runs of spaces and trim.
    std::string out;
    bool last_space = true;
    for (char c : s) {
        if (c == ' ') {
            if (!last_space) { out += ' '; last_space = true; }
        } else {
            out += c;
            last_space = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool contains_word(const std::string& s, const std::string& w) {
    return replace_word(s, w, "##__MARKER__##") != s;
}

// Returns "kingside" / "queenside" / "" depending on the utterance.
std::string detect_castling(const std::string& s) {
    if (s == "o o" || s == "oo" || s == "o-o") return "kingside";
    if (s == "o o o" || s == "ooo" || s == "o-o-o") return "queenside";
    bool has_castle = contains_word(s, "castle") ||
                      contains_word(s, "castles") ||
                      contains_word(s, "castling");
    if (has_castle && contains_word(s, "kingside")) return "kingside";
    if (has_castle && contains_word(s, "queenside")) return "queenside";
    if (contains_word(s, "short") && contains_word(s, "castle")) return "kingside";
    if (contains_word(s, "long") && contains_word(s, "castle"))  return "queenside";
    return "";
}

PieceType piece_from_word(const std::string& w) {
    if (w == "king")   return KING;
    if (w == "queen")  return QUEEN;
    if (w == "rook")   return ROOK;
    if (w == "bishop") return BISHOP;
    if (w == "knight") return KNIGHT;
    if (w == "pawn")   return PAWN;
    return PIECE_COUNT;
}

const char* piece_name(PieceType t) {
    switch (t) {
        case KING:   return "king";
        case QUEEN:  return "queen";
        case ROOK:   return "rook";
        case BISHOP: return "bishop";
        case KNIGHT: return "knight";
        case PAWN:   return "pawn";
        default:     return "?";
    }
}

std::vector<std::string> tokenise(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string t;
    while (iss >> t) out.push_back(t);
    return out;
}

// Find a destination square. Accepts either a single 2-char token
// like "e4" or two adjacent tokens "e" + "4" (the spelled-digit form
// "e four" tokenises to two separate tokens after normalisation).
// Returns the 1-based index of the LAST token consumed (so the
// caller's disambig loop knows to skip both halves of a pair) and
// fills file (0..7) and rank (0..7).
size_t find_destination(const std::vector<std::string>& toks,
                        int& file_out, int& rank_out,
                        size_t* first_idx_out = nullptr) {
    for (size_t i = 0; i < toks.size(); ++i) {
        const std::string& t = toks[i];
        if (t.size() == 2 &&
            t[0] >= 'a' && t[0] <= 'h' &&
            t[1] >= '1' && t[1] <= '8') {
            file_out = t[0] - 'a';
            rank_out = t[1] - '1';
            if (first_idx_out) *first_idx_out = i;
            return i + 1;
        }
        if (t.size() == 1 && t[0] >= 'a' && t[0] <= 'h' &&
            i + 1 < toks.size() &&
            toks[i+1].size() == 1 &&
            toks[i+1][0] >= '1' && toks[i+1][0] <= '8') {
            file_out = t[0] - 'a';
            rank_out = toks[i+1][0] - '1';
            if (first_idx_out) *first_idx_out = i;
            return i + 2;
        }
    }
    return 0;
}

// External (a=0..h=7) → internal column (a=7..h=0).
int file_to_col(int f) { return 7 - f; }

}  // namespace

bool parse_voice_move(const std::string& utterance,
                      const GameState& gs,
                      std::string& uci_out,
                      std::string& error_out) {
    uci_out.clear();
    error_out.clear();

    std::string s = normalize(utterance);
    if (s.empty()) {
        error_out = "Empty utterance";
        return false;
    }

    // Castling first — distinct grammar.
    std::string castle = detect_castling(s);
    if (!castle.empty()) {
        GameState scratch = gs;
        int king_row = gs.white_turn ? 0 : 7;
        int king_col = file_to_col(4);                  // e-file
        int dest_col = file_to_col(castle == "kingside" ? 6 : 2);  // g- or c-file
        auto legal = generate_legal_moves(scratch, king_col, king_row);
        bool ok = false;
        for (const auto& m : legal) {
            if (m.first == dest_col && m.second == king_row) { ok = true; break; }
        }
        if (!ok) {
            error_out = std::string("Cannot castle ") + castle + " right now";
            return false;
        }
        uci_out = move_to_uci(king_col, king_row, dest_col, king_row);
        return true;
    }

    auto toks = tokenise(s);

    // First piece keyword in the utterance wins. ("pawn e8 queen" =
    // pawn moving to e8 which auto-queens; the trailing piece is ignored.)
    PieceType piece = PIECE_COUNT;
    for (const auto& t : toks) {
        PieceType p = piece_from_word(t);
        if (p != PIECE_COUNT) { piece = p; break; }
    }

    int file = -1, rank = -1;
    size_t dest_first = 0;
    size_t dest_idx_1 = find_destination(toks, file, rank, &dest_first);
    if (!dest_idx_1) {
        error_out = std::string("No destination square heard in '") + utterance + "'";
        return false;
    }

    // Source disambiguator: any single-letter file token (a..h) other
    // than the destination's file token, or any single-digit rank
    // token. Skip the entire destination span (1 or 2 tokens).
    int src_file = -1, src_rank = -1;
    for (size_t i = 0; i < toks.size(); ++i) {
        if (i >= dest_first && i < dest_idx_1) continue;
        const std::string& t = toks[i];
        if (t.size() == 1) {
            char c = t[0];
            if (c >= 'a' && c <= 'h')      src_file = c - 'a';
            else if (c >= '1' && c <= '8') src_rank = c - '1';
        }
    }

    if (piece == PIECE_COUNT) piece = PAWN;

    int dest_col = file_to_col(file);
    int dest_row = rank;

    GameState scratch = gs;
    std::vector<std::pair<int,int>> matches;  // (from_col, from_row)
    for (const auto& p : gs.pieces) {
        if (!p.alive || p.is_white != gs.white_turn) continue;
        if (p.type != piece) continue;
        if (src_file >= 0 && p.col != file_to_col(src_file)) continue;
        if (src_rank >= 0 && p.row != src_rank) continue;
        auto legal = generate_legal_moves(scratch, p.col, p.row);
        for (const auto& m : legal) {
            if (m.first == dest_col && m.second == dest_row) {
                matches.push_back({p.col, p.row});
                break;
            }
        }
    }

    if (matches.empty()) {
        error_out = std::string("Illegal or unrecognised: '") + utterance + "'";
        return false;
    }
    if (matches.size() > 1) {
        std::string list;
        for (size_t i = 0; i < matches.size(); ++i) {
            if (i) list += ", ";
            list += static_cast<char>('a' + (7 - matches[i].first));
        }
        error_out = std::string("Ambiguous: ") + std::to_string(matches.size()) +
                    " " + piece_name(piece) + "s reach the destination (" +
                    list + "); add a source file";
        return false;
    }

    uci_out = move_to_uci(matches[0].first, matches[0].second, dest_col, dest_row);
    return true;
}

// ---------------------------------------------------------------------------
// UI-button command parser
// ---------------------------------------------------------------------------
namespace {

// Normalise a transcribed utterance for command matching: lowercase,
// drop everything that isn't a letter or whitespace (apostrophes,
// punctuation, digits — none of our button labels contain digits),
// collapse runs of whitespace, trim.
std::string normalize_command(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool last_space = true;
    for (char c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isalpha(u)) {
            out.push_back(static_cast<char>(std::tolower(u)));
            last_space = false;
        } else if (std::isspace(u)) {
            if (!last_space) {
                out.push_back(' ');
                last_space = true;
            }
        }
        // skip anything else
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool match_any(const std::string& s,
               std::initializer_list<const char*> phrases) {
    for (const char* p : phrases)
        if (s == p) return true;
    return false;
}

bool is_back_phrase(const std::string& s) {
    return match_any(s, {
        "back", "back to menu", "back to the menu",
        "back to main menu", "main menu", "menu",
        "go back", "exit", "quit", "leave",
    });
}

}  // namespace

VoiceCommand parse_voice_command(const std::string& utterance,
                                 const VoiceCommandContext& ctx) {
    std::string s = normalize_command(utterance);
    if (s.empty()) return VoiceCommand::None;

    // Modal confirmation eats every utterance — same way the
    // withdraw modal eats every mouse click. Only yes/no register;
    // anything else returns None and the caller short-circuits the
    // chess-move parser so we don't surface "no destination square".
    if (ctx.withdraw_confirm_open || ctx.hint_confirm_open) {
        if (match_any(s, {"yes", "yeah", "yep", "yup",
                          "confirm", "ok", "okay", "sure",
                          "do it", "go ahead", "play it",
                          "play that"}))
            return VoiceCommand::ConfirmYes;
        if (match_any(s, {"no", "nope", "nah",
                          "cancel", "never mind", "nevermind",
                          "stop", "abort"}))
            return VoiceCommand::ConfirmNo;
        return VoiceCommand::None;
    }

    switch (ctx.mode) {
    case MODE_MENU:
        if (match_any(s, {"play", "start", "new game", "begin",
                          "go", "start game", "play chess"}))
            return VoiceCommand::StartGame;
        if (match_any(s, {"challenges", "challenge",
                          "homework", "tactics", "exercises"}))
            return VoiceCommand::OpenChallenges;
        if (match_any(s, {"puzzles", "puzzle", "puzzle of the day",
                          "daily puzzle", "chess dot com puzzle"}))
            return VoiceCommand::OpenPuzzles;
        if (match_any(s, {"options", "settings", "preferences"}))
            return VoiceCommand::OpenOptions;
        break;

    case MODE_PREGAME:
        if (match_any(s, {"start", "play", "go", "begin",
                          "start game", "lets go", "lets start"}))
            return VoiceCommand::StartGame;
        if (is_back_phrase(s)) return VoiceCommand::BackToMenu;
        if (match_any(s, {"white", "play white", "im white",
                          "i play white", "i am white"}))
            return VoiceCommand::PlayWhite;
        if (match_any(s, {"black", "play black", "im black",
                          "i play black", "i am black"}))
            return VoiceCommand::PlayBlack;
        break;

    case MODE_OPTIONS:
        if (is_back_phrase(s)) return VoiceCommand::BackToMenu;
        if (match_any(s, {"cartoon outline", "outline", "cartoon",
                          "toggle outline", "toggle cartoon",
                          "toggle cartoon outline"}))
            return VoiceCommand::ToggleCartoonOutline;
        if (match_any(s, {"continuous voice", "voice mode",
                          "toggle voice", "voice on", "voice off",
                          "voice"}))
            return VoiceCommand::ToggleContinuousVoice;
        if (match_any(s, {"robotic board", "robot board", "robotic",
                          "physical board",
                          "chessnut", "chessnut move", "phantom",
                          "phantom chessboard", "phantom board",
                          "toggle robotic board", "toggle robot",
                          "toggle chessnut", "toggle move"}))
            return VoiceCommand::ToggleChessnut;
        if (match_any(s, {"verbose log", "verbose", "ble verbose",
                          "ble log", "verbose ble", "toggle verbose",
                          "debug log"}))
            return VoiceCommand::ToggleBleVerbose;
        if (match_any(s, {"speak moves", "announce moves",
                          "toggle speak", "toggle voice output",
                          "speak", "announce"}))
            return VoiceCommand::ToggleSpeakMoves;
        // Tri-state cycle (Off → Auto → OnDemand → Off).
        if (match_any(s, {"move hints", "hint mode", "toggle hints",
                          "cycle hints", "coach mode"}))
            return VoiceCommand::ToggleHints;
        break;

    case MODE_PLAYING:
        if (ctx.game_over || ctx.analysis_mode) {
            if (is_back_phrase(s)) return VoiceCommand::BackToMenu;
            if (ctx.analysis_mode &&
                match_any(s, {"continue playing", "keep playing",
                              "play on", "continue", "resume"}))
                return VoiceCommand::ContinuePlaying;
            if (ctx.game_over && !ctx.analysis_mode &&
                match_any(s, {"new game", "play again", "rematch",
                              "new match", "again"}))
                return VoiceCommand::NewGame;
        } else {
            // Live game — same set of commands the withdraw flag
            // exposes via mouse click. Opens the confirmation modal.
            if (match_any(s, {"resign", "withdraw", "give up",
                              "i resign", "i give up", "surrender",
                              "raise the flag", "wave the flag"}))
                return VoiceCommand::Resign;
            // One-shot hint request — works in any hint mode but
            // only surfaces a hint when the mode isn't Off (the
            // dispatcher in app_state.cpp handles that gating).
            if (match_any(s, {"give me a hint", "show me a hint",
                              "hint please", "show hint", "hint",
                              "what should i play",
                              "what is the best move",
                              "what's the best move",
                              "best move"}))
                return VoiceCommand::RequestHint;
        }
        break;

    case MODE_CHALLENGE_SELECT:
        if (is_back_phrase(s)) return VoiceCommand::BackToMenu;
        break;

    case MODE_CHALLENGE:
        if (ctx.challenge_show_summary) {
            if (is_back_phrase(s) ||
                match_any(s, {"done", "ok", "okay", "finish"}))
                return VoiceCommand::BackToMenu;
            break;
        }
        if (ctx.challenge_solved &&
            match_any(s, {"next", "next puzzle", "next challenge",
                          "continue", "go next"}))
            return VoiceCommand::NextPuzzle;
        if (ctx.challenge_mistake_ready &&
            match_any(s, {"try again", "retry", "again",
                          "restart", "reset"}))
            return VoiceCommand::TryAgain;
        if (is_back_phrase(s)) return VoiceCommand::BackToMenu;
        break;

    default:
        break;
    }
    return VoiceCommand::None;
}
