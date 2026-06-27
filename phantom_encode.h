#pragma once

// Pure-logic Phantom Chessboard wire-format encoder (firmware v0.3.0).
// Header-only so the desktop SimpleBLE impl (phantom_bridge.cpp) and the web
// Web-Bluetooth impl (web/chessnut_web.cpp) share one source of truth.
//
// See docs/PHANTOM.md for the protocol write-up and
// ~/claude_workspace/scratchpad/phantom_chessboard/PHANTOM_V030_PROTOCOL.md for
// the authoritative decompile-derived spec it was lifted from.
//
// ── Protocol in one paragraph ───────────────────────────────────────────────
// The current Phantom firmware (the board reports version "0.3.0" on its
// version characteristic) speaks an *opcode-framed* protocol over a single
// "game" characteristic (`GAME_UUID`). Every frame is `[opcode_byte] + ASCII`.
// A move is pushed app→board as two frames — a `side` frame (`"2"`) then a
// `movement` frame (`"M e2-e4 P"`). The board reports a detected physical move
// board→app as a single `movement` (opcode 0x06) notification carrying
// `"e2-e4 …"` in plain algebraic. The board only starts reporting once the app
// writes the play-mode digit `"2"` to the separate mode characteristic
// (`MODE_UUID`). This entirely replaces the older single-write `"M 1 e2-e4"`
// ASCII scheme (still documented below as the LEGACY_* constants), which lived
// on a different GATT layout that production boards OTA past.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace phantom {

// ===========================================================================
// GATT — firmware v0.3.0. One primary service; the move protocol uses just the
// GAME + MODE characteristics. UUIDs verified against a live board
// (`Phantom 3579`) AND the decompiled app (`BLEManager` in utils/uuid.dart).
// ===========================================================================
constexpr const char* SERVICE_UUID =
    "fd31a840-22e7-11eb-adc1-0242ac120002";

// The game channel ("UUID_GAME"). The app writes every opcode frame here and
// subscribes here for inbound move / board-state notifications. Flags R+W+N.
constexpr const char* GAME_UUID =
    "cc68a66e-3bfa-4614-a77f-f46954a4c103";

// Mode / control. A plain ASCII digit is written here (no opcode byte):
// "2" = enter play mode (board begins reporting sensor moves), "3" = end/idle.
constexpr const char* MODE_UUID =
    "c08d3691-e60f-4467-b2d0-4a4b7c72777e";

// Matrix-validation result push ("UUID_SEND_MATRIX" in the app — a misnomer;
// the matrix bytes go out on GAME_UUID, this only carries the board's
// validation *status* string, e.g. "ERROR1(43)" / "CLEAN"). Notify only.
constexpr const char* MATRIX_RESULT_UUID =
    "1b034927-77e8-433e-ac4c-27302e5e853f";

// User-warning text push (notify only) — surfaced as a dialog in the app, not
// chess data. We subscribe so the raw text reaches the verbose log.
constexpr const char* WARNING_UUID =
    "1b034928-77e8-433e-ac4c-27302e5e853f";

// Read-only informational characteristics (handy for diagnostics).
constexpr const char* VERSION_UUID =
    "392d9e66-937a-11ee-b9d1-0242ac120002";  // "0.3.0"
constexpr const char* BATTERY_UUID =
    "7b204548-40c4-11eb-adc1-0242ac120002";  // "<counter>,0,1,1"

// Firmware OTA — NEVER write this characteristic from the driver.
constexpr const char* OTA_UUID =
    "93601602-bbc2-4e53-95bd-a3ba326bc04b";

// ── Legacy (pre-0.3.0 firmware) — kept only so the bridge can recognise an old
// board and so docs/tooling have one reference. The driver targets v0.3.0. ──
constexpr const char* LEGACY_MOVE_CMD_UUID =
    "7b204548-30c3-11eb-adc1-0242ac120002";
constexpr const char* LEGACY_DETECTED_MOVE_UUID =
    "06034924-77e8-433e-ac4c-27302e5e853f";

// ===========================================================================
// GameOPCode — the frame-leading opcode byte. Integer values read directly
// from the app's enum dump (objs.txt). Only a handful are used by this driver;
// the rest are listed for completeness / future use.
// ===========================================================================
enum class GameOp : uint8_t {
    GameStart      = 0x00,  // + "<100-char matrix>,W|B" : full-position sync
    GameEnd        = 0x01,
    Movement       = 0x02,  // + "M e2-e4 P" : drive a move (also inbound 0x06)
    MovementVerify = 0x03,  // + "1"/"2" : accept/reject a detected move
    VoiceCommand   = 0x04,
    Takeback       = 0x05,
    Calibration    = 0x07,
    CheckSound     = 0x09,
    Side           = 0x0a,  // + "2" : sent right before each Movement frame
    GameAssistance = 0x0b,
    SnapToCenter   = 0x0d,
    ResetDetection = 0x0e,
    ErrorMsg       = 0x10,  // inbound only: payload = matrix-validation string
};

// Inbound notifications on GAME_UUID dispatch on byte 0:
constexpr uint8_t INBOUND_MOVE_OPCODE        = 0x06;  // detected physical move
constexpr uint8_t INBOUND_BOARDSTATE_OPCODE  = 0x0c;  // board-state update
constexpr uint8_t INBOUND_ERRORMSG_OPCODE    = 0x10;  // matrix-validation text

// Mode-characteristic digit tokens (plain ASCII, written to MODE_UUID).
constexpr const char* MODE_PLAY = "2";  // enter play mode
constexpr const char* MODE_END  = "3";  // end / idle

// Move-string punctuation.
constexpr char SEP_NORMAL  = '-';
constexpr char SEP_CAPTURE = 'x';

// ===========================================================================
// Framing — every GAME_UUID write is `[opcode] + ASCII payload`.
// ===========================================================================
inline std::vector<uint8_t> make_frame(GameOp op, const std::string& ascii) {
    std::vector<uint8_t> out;
    out.reserve(ascii.size() + 1);
    out.push_back(static_cast<uint8_t>(op));
    for (unsigned char c : ascii) out.push_back(c);
    return out;
}

// ===========================================================================
// Outbound move (app → board). A move is two frames written back-to-back to
// GAME_UUID, exactly as the official app's sendBleMoveComm does:
//   1) Side    : [0x0a] + "2"
//   2) Movement: [0x02] + "M <from><sep><to> <piece>"
// `piece` is the FEN letter of the moved piece (white "PNBRQK", black
// "pnbrqk"); pass 'E' if unknown — the firmware tracks pieces itself, so the
// letter is advisory. `capture` selects '-' vs 'x' for the separator.
// ===========================================================================
inline std::string move_text(int src_col, int src_row,
                             int dst_col, int dst_row,
                             bool capture, char piece) {
    auto file_char = [](int col) -> char {
        return (col >= 0 && col < 8) ? static_cast<char>('a' + col) : '?';
    };
    auto rank_char = [](int row) -> char {
        return (row >= 0 && row < 8) ? static_cast<char>('1' + row) : '?';
    };
    std::string s = "M ";
    s.push_back(file_char(src_col));
    s.push_back(rank_char(src_row));
    s.push_back(capture ? SEP_CAPTURE : SEP_NORMAL);
    s.push_back(file_char(dst_col));
    s.push_back(rank_char(dst_row));
    s.push_back(' ');
    s.push_back(piece == 0 ? 'E' : piece);
    return s;  // e.g. "M e2-e4 P"
}

// The constant leading "side" frame.
inline std::vector<uint8_t> make_side_frame() {
    return make_frame(GameOp::Side, "2");
}

// The movement frame for a single move.
inline std::vector<uint8_t> make_movement_frame(int src_col, int src_row,
                                                int dst_col, int dst_row,
                                                bool capture, char piece) {
    return make_frame(GameOp::Movement,
                      move_text(src_col, src_row, dst_col, dst_row,
                                capture, piece));
}

// Convenience: the full two-frame move sequence as a pair.
inline std::array<std::vector<uint8_t>, 2> make_move_frames(
        int src_col, int src_row, int dst_col, int dst_row,
        bool capture, char piece) {
    return { make_side_frame(),
             make_movement_frame(src_col, src_row, dst_col, dst_row,
                                  capture, piece) };
}

// ===========================================================================
// Inbound detected-move parse (board → app). A physical move arrives as a
// notification on GAME_UUID: `[0x06] + ASCII "<from><sep><to> <t2> <t3>"`.
// We only need the first whitespace-delimited token (the move); the trailing
// tokens are unused by the official app too. Out-params receive the squares in
// file/rank indices (col 0 = a-file, row 0 = rank 1). Returns false on any
// frame that isn't a well-formed move (wrong opcode, short, bad chars).
// ===========================================================================
inline bool parse_detected_move(const uint8_t* frame, size_t len,
                                int& src_col, int& src_row,
                                int& dst_col, int& dst_row,
                                bool& is_capture) {
    if (len < 6) return false;
    if (frame[0] != INBOUND_MOVE_OPCODE) return false;
    // ASCII payload starts at byte 1; isolate the first space-delimited token.
    std::string s(reinterpret_cast<const char*>(frame + 1), len - 1);
    size_t sp = s.find(' ');
    std::string mv = (sp == std::string::npos) ? s : s.substr(0, sp);
    if (mv.size() < 5) return false;
    char sf = mv[0], sr = mv[1], sep = mv[2], df = mv[3], dr = mv[4];
    if (sf < 'a' || sf > 'h' || df < 'a' || df > 'h') return false;
    if (sr < '1' || sr > '8' || dr < '1' || dr > '8') return false;
    if (sep != SEP_NORMAL && sep != SEP_CAPTURE) return false;
    src_col = sf - 'a'; src_row = sr - '1';
    dst_col = df - 'a'; dst_row = dr - '1';
    is_capture = (sep == SEP_CAPTURE);
    return true;
}

// ===========================================================================
// FEN piece lookup. Return the FEN piece letter at (file 0..7 = a..h,
// rank 0..7 = 1..8) in the board field of `fen`, or 0 if the square is empty
// or out of range. The FEN board field lists rank 8 first, files a..h within
// each rank. Used to fill the advisory piece letter in an outbound move frame.
// ===========================================================================
inline char fen_piece_at(const std::string& fen, int file, int rank) {
    if (file < 0 || file > 7 || rank < 0 || rank > 7) return 0;
    size_t sp = fen.find(' ');
    std::string bd = (sp == std::string::npos) ? fen : fen.substr(0, sp);
    const int want_line = 7 - rank;      // 0 = rank 8 (first '/'-segment)
    int line = 0;
    size_t i = 0;
    while (line < want_line && i < bd.size()) {
        if (bd[i] == '/') line++;
        i++;
    }
    int f = 0;                            // file 0 = 'a'
    for (; i < bd.size() && bd[i] != '/'; i++) {
        char c = bd[i];
        if (c >= '1' && c <= '8') {
            f += c - '0';
        } else {
            if (f == file) return c;
            f++;
        }
        if (f > file) return 0;           // target fell in an empty run
    }
    return 0;
}

// ===========================================================================
// True iff the given device-name string looks like a Phantom board. Matched
// case-insensitively against advertising names — covers the two brand variants
// the firmware string table mentions.
// ===========================================================================
inline bool is_phantom_name(const std::string& name) {
    std::string l;
    l.reserve(name.size());
    for (char c : name) {
        l.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
    }
    return l.find("phantom") != std::string::npos
        || l.find("gochess") != std::string::npos;
}

}  // namespace phantom
