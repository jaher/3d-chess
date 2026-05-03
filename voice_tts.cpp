// voice_tts.cpp — pure-logic SAN-to-spoken-English conversion.
//
// Platform glue (espeak-ng on desktop, speechSynthesis on web)
// lives in voice_tts_native.cpp / web/voice_tts_web.cpp; this
// translation unit has no platform deps and is included in the
// pure-logic test binary.

#include "voice_tts.h"

#include "chess_rules.h"
#include "chess_types.h"

#include <cctype>
#include <string>

namespace {

// "f" → "f", "3" → "three". Files stay as their letter (espeak-ng
// pronounces single letters cleanly); ranks become words so "f3"
// reads "f three" not "f thirty-three" / "f three" (sometimes
// espeak fuses adjacent digits when the file is reduced to a
// vowel-sounding letter like "e" / "a").
const char* rank_word(char digit) {
    switch (digit) {
    case '1': return "one";
    case '2': return "two";
    case '3': return "three";
    case '4': return "four";
    case '5': return "five";
    case '6': return "six";
    case '7': return "seven";
    case '8': return "eight";
    default:  return "";
    }
}

// Piece prefix in SAN ('K','Q','R','B','N') → spoken word. Lower-
// case 'p' is unused in SAN — pawn moves omit the prefix entirely.
const char* piece_word(char san_prefix) {
    switch (san_prefix) {
    case 'K': return "King";
    case 'Q': return "Queen";
    case 'R': return "Rook";
    case 'B': return "Bishop";
    case 'N': return "Knight";
    default:  return "";
    }
}

// Spell out a single square: "e4" → "e four".
void append_square(std::string& out, char file, char rank) {
    out += file;
    out += ' ';
    out += rank_word(rank);
}

// Pull check / checkmate suffix off the SAN tail before parsing
// the body. Returns the suffix word (or empty) and trims the chars
// from `san` in place.
std::string extract_check_suffix(std::string& san) {
    if (san.empty()) return {};
    char last = san.back();
    if (last == '#') { san.pop_back(); return ", checkmate"; }
    if (last == '+') { san.pop_back(); return ", check"; }
    return {};
}

}  // namespace

std::string san_to_speech(const std::string& san_in) {
    std::string san = san_in;
    if (san.empty()) return san_in;
    std::string suffix = extract_check_suffix(san);

    // Castling — handle longest match first so O-O-O isn't truncated
    // to O-O.
    if (san == "O-O-O" || san == "0-0-0") return "Castles queenside" + suffix;
    if (san == "O-O"   || san == "0-0")   return "Castles kingside"  + suffix;

    // Identify the piece prefix (uppercase letter at index 0). Pawn
    // moves have no prefix and start with a file letter ('a'..'h').
    bool is_pawn = !san.empty() && san[0] >= 'a' && san[0] <= 'h';
    char piece = is_pawn ? 'p' : san[0];
    size_t i = is_pawn ? 0 : 1;

    // SAN body roughly looks like:
    //   pawn:    "e4", "exd5", "exd5=Q", "e8=Q"
    //   piece:   "Nf3", "Nbd2", "N1f3", "Nbxd5", "Qh4"
    //   So between the piece prefix and the destination square there
    //   may be:
    //     - a disambiguation char ('a'..'h' or '1'..'8'), maybe two
    //     - an 'x' for capture
    //   The destination is always file+rank ([a-h][1-8]) followed by
    //   optional "=Q" / "=R" / "=N" / "=B" promotion.
    bool capture = false;
    char disambig_file = 0;  // 0 = none
    char disambig_rank = 0;
    char dest_file = 0, dest_rank = 0;
    char promote = 0;

    // Walk forward collecting capture flag + disambiguation, then
    // the last [a-h][1-8] in the body is the destination.
    // Strategy: find the LAST [a-h][1-8] pair as the dest, treat
    // anything before it as disambiguation.
    int last_pair = -1;
    for (size_t j = i; j + 1 < san.size(); ++j) {
        char a = san[j], b = san[j + 1];
        if (a >= 'a' && a <= 'h' && b >= '1' && b <= '8') {
            last_pair = static_cast<int>(j);
        }
    }
    if (last_pair < 0) return san_in + suffix;  // unparseable, return as-is
    dest_file = san[last_pair];
    dest_rank = san[last_pair + 1];

    // Promotion suffix: "=Q" right after the destination square.
    if (last_pair + 3 < static_cast<int>(san.size()) &&
        san[last_pair + 2] == '=') {
        promote = san[last_pair + 3];
    }

    // Anything in [i, last_pair) is disambiguation + 'x'.
    for (int j = static_cast<int>(i); j < last_pair; ++j) {
        char c = san[j];
        if (c == 'x') capture = true;
        else if (c >= 'a' && c <= 'h') disambig_file = c;
        else if (c >= '1' && c <= '8') disambig_rank = c;
    }

    // Build the spoken phrase.
    //
    // Sentence order: <piece> [from <disambig>] <to|takes> <dest>
    //                       [, promotes to <piece>] [, check|checkmate]
    //
    // Pawn captures in SAN always carry the source file ("exd5"),
    // so they flow through the same disambiguation arm as piece
    // moves: "Pawn from e takes d five".
    std::string out;
    if (is_pawn) {
        out = "Pawn ";
    } else {
        out = piece_word(piece);
        out += ' ';
    }

    if (disambig_file || disambig_rank) {
        out += "from ";
        if (disambig_file) {
            out += disambig_file;
            if (disambig_rank) out += ' ';
        }
        if (disambig_rank) {
            // Bare rank disambig is rare ("R1a3"); spell "rank one"
            // so the listener doesn't confuse it with a destination.
            if (!disambig_file) out += "rank ";
            out += rank_word(disambig_rank);
        }
        out += ' ';
    }

    if (capture) out += "takes ";
    else         out += "to ";

    append_square(out, dest_file, dest_rank);

    if (promote) {
        out += ", promotes to ";
        switch (std::toupper(static_cast<unsigned char>(promote))) {
        case 'Q': out += "queen";  break;
        case 'R': out += "rook";   break;
        case 'B': out += "bishop"; break;
        case 'N': out += "knight"; break;
        default:  out += "queen";  break;
        }
    }

    out += suffix;
    return out;
}

std::string uci_to_speech(const BoardSnapshot& before,
                          const std::string& uci) {
    // Reuse the existing SAN renderer (handles disambiguation,
    // captures, castling, en passant rendered as `exd5`, promotion,
    // check / mate suffix) then translate to spoken English.
    return san_to_speech(uci_to_algebraic(before, uci));
}
