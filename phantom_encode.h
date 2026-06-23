#pragma once

// Pure-logic Phantom Chessboard wire-format encoder. Header-only so
// the desktop SimpleBLE impl (phantom_bridge.cpp) and the web
// Web-Bluetooth impl (web/phantom_web.cpp) share one source of truth.
//
// See docs/PHANTOM.md for the reverse-engineering notes.
// In short: Phantom is an ESP32 robotic chessboard. The app drives
// motors by writing a short ASCII move string to characteristic
// 7b204548-… (`MOVE_CMD_UUID` below); the firmware's Play-Mode loop
// pulls it, parses bytes 2..6 (`[file][rank][-/x][file][rank]`),
// looks up the piece on the source square in its own piece tracker,
// and dispatches to moveChessPiece → stepper drive.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace phantom {

// ===========================================================================
// GATT — single primary service, 19 characteristics. The driver
// only needs a handful; the rest are passive containers / NVS
// configuration / OTA.
// ===========================================================================
constexpr const char* SERVICE_UUID =
    "fd31a840-22e7-11eb-adc1-0242ac120002";

// Motor-drive write target. App writes a 7..25 byte string here;
// firmware's Play-Mode loop consumes it. Property byte 0x0a (R+W).
constexpr const char* MOVE_CMD_UUID =
    "7b204548-30c3-11eb-adc1-0242ac120002";

// Detected-move push channel — VERIFIED from firmware. Decompiling
// FUN_400d2c3c (the move pusher) showed it writes the 9-byte
// `"M 1 e2-e4"` frame onto chr-pointer DAT_400d00dc, which
// FUN_400d2594 (GATT setup) initialises with this UUID and props
// 0x12 (R+N). FUN_400d5acc builds the buffer with a literal
// `"M 1 "` prefix via `builtin_strncpy(buf, "M 1 ", 4)` followed
// by file/rank/sep/file/rank.
constexpr const char* NOTIFY_DETECTED_MOVE_UUID =
    "06034924-77e8-433e-ac4c-27302e5e853f";  // R+N — detected-move push

// Other notify-capable chars. The driver subscribes to all of them
// and logs anything that lands; only NOTIFY_DETECTED_MOVE_UUID
// frames currently feed the digital game.
constexpr const char* NOTIFY_LEGACY_STATUS_UUID =
    "7b204d4a-30c3-11eb-adc1-0242ac120002";  // R+N (legacy "status")
constexpr const char* NOTIFY_RWN_UUID =
    "c08d3691-e60f-4467-b2d0-4a4b7c72777e";  // R+W+N main bidi
constexpr const char* NOTIFY_VERSION_UUID =
    "acb65af4-92ca-11ee-b9d1-0242ac120002";  // R+N (version)
constexpr const char* NOTIFY_OTA_UUID =
    "93601602-bbc2-4e53-95bd-a3ba326bc04b";  // W+N (OTA progress)

// All five, in the order we subscribe — the desktop and web
// drivers iterate this list to keep parity.
constexpr std::array<const char*, 5> NOTIFY_UUIDS = {
    NOTIFY_DETECTED_MOVE_UUID,
    NOTIFY_LEGACY_STATUS_UUID,
    NOTIFY_RWN_UUID,
    NOTIFY_VERSION_UUID,
    NOTIFY_OTA_UUID,
};

// Detected-move notify-frame format (board → app). Verified.
constexpr size_t      DETECTED_MOVE_FRAME_LEN = 9;
constexpr const char* DETECTED_MOVE_PREFIX    = "M 1 ";
constexpr size_t      DETECTED_MOVE_PREFIX_LEN = 4;
// Byte indices within the 9-byte frame:
constexpr size_t DETECTED_MOVE_OFF_SRC_FILE = 4;
constexpr size_t DETECTED_MOVE_OFF_SRC_RANK = 5;
constexpr size_t DETECTED_MOVE_OFF_SEP      = 6;  // '-' or 'x'
constexpr size_t DETECTED_MOVE_OFF_DST_FILE = 7;
constexpr size_t DETECTED_MOVE_OFF_DST_RANK = 8;

// Parse a 9-byte board→app detected-move frame into UCI-ish
// components. Returns true on a well-formed frame ('a'..'h' for
// files, '1'..'8' for ranks, '-' or 'x' separator). Out-params
// receive the squares in the project's internal coords (col 0 =
// a-file, row 0 = rank 1).
inline bool parse_detected_move(const uint8_t* frame, size_t len,
                                int& src_col, int& src_row,
                                int& dst_col, int& dst_row,
                                bool& is_capture) {
    if (len != DETECTED_MOVE_FRAME_LEN) return false;
    if (frame[0] != 'M' || frame[1] != ' ' || frame[3] != ' ') return false;
    char sf = static_cast<char>(frame[DETECTED_MOVE_OFF_SRC_FILE]);
    char sr = static_cast<char>(frame[DETECTED_MOVE_OFF_SRC_RANK]);
    char sp = static_cast<char>(frame[DETECTED_MOVE_OFF_SEP]);
    char df = static_cast<char>(frame[DETECTED_MOVE_OFF_DST_FILE]);
    char dr = static_cast<char>(frame[DETECTED_MOVE_OFF_DST_RANK]);
    if (sf < 'a' || sf > 'h' || df < 'a' || df > 'h') return false;
    if (sr < '1' || sr > '8' || dr < '1' || dr > '8') return false;
    if (sp != '-' && sp != 'x') return false;
    src_col = sf - 'a'; src_row = sr - '1';
    dst_col = df - 'a'; dst_row = dr - '1';
    is_capture = (sp == 'x');
    return true;
}

// Move-validation echo. The Play-Mode loop polls this for "1" (ok)
// or "2" (reject) after pushing a sensor-detected move out to the
// app. The driver doesn't write here yet (we don't have a verified
// format) but exposes the UUID so the integration layer can.
constexpr const char* CHECK_MOVE_UUID =
    "06034924-77e8-433e-ac4c-27302e5e853f";

// Mode select. Persisted to NVS under `myApp/`. The official app
// writes a mode token (Lichess / Chess.com / offline-AI / 2P) when
// the user picks a mode. Not used by this driver.
constexpr const char* MODE_SELECT_UUID =
    "c60c786b-bf3f-49d8-bd9e-c268e0519a7b";

// ===========================================================================
// MOVE_CMD wire format
// ===========================================================================
// Total length: 7..25 bytes (firmware rejects ≥26 with "error -1").
// The Play-Mode loop's parser indexes byte offsets 2..6 of the
// stored string and does not validate bytes 0..1. We send a
// 2-byte `"M "` prefix matching the leading two bytes of the
// outbound detected-move frame (`"M 1 e2-e4"`) — the firmware-side
// design is plausibly "M means move, the rest is the move text",
// with the "1" on outbound being a counter/identifier the inbound
// path doesn't use. Either way the firmware ignores bytes 0..1, so
// the worst case is harmless.
//
// Byte 0..1: prefix `"M "` (2 bytes — see comment above).
// Byte 2:    src file 'a'..'h'
// Byte 3:    src rank '1'..'8'
// Byte 4:    '-' (normal move) or 'x' (capture)
// Byte 5:    dst file
// Byte 6:    dst rank
// Byte 7+:   ignored / padding
constexpr const char* MOVE_CMD_PREFIX = "M ";
constexpr size_t      MOVE_CMD_PREFIX_LEN = 2;
constexpr char        MOVE_CMD_NORMAL_SEP = '-';
constexpr char        MOVE_CMD_CAPTURE_SEP = 'x';

// Build a Phantom MOVE_CMD payload from a UCI-style move.
// `capture` selects between '-' and 'x' for byte 4. Promotion
// suffix in UCI ("e7e8q") is dropped — the firmware resolves
// promotion internally based on its piece tracker.
inline std::string make_move_cmd(int src_col, int src_row,
                                 int dst_col, int dst_row,
                                 bool capture) {
    auto file_char = [](int col) -> char {
        return (col >= 0 && col < 8) ? static_cast<char>('a' + col) : '?';
    };
    auto rank_char = [](int row) -> char {
        return (row >= 0 && row < 8) ? static_cast<char>('1' + row) : '?';
    };
    std::string s = MOVE_CMD_PREFIX;            // "M "
    s.push_back(file_char(src_col));
    s.push_back(rank_char(src_row));
    s.push_back(capture ? MOVE_CMD_CAPTURE_SEP : MOVE_CMD_NORMAL_SEP);
    s.push_back(file_char(dst_col));
    s.push_back(rank_char(dst_row));
    return s;  // 7 bytes — firmware accepts 7..25
}

inline std::string make_move_cmd_uci(const std::string& uci, bool capture) {
    if (uci.size() < 4) return std::string{};
    int sc = uci[0] - 'a', sr = uci[1] - '1';
    int dc = uci[2] - 'a', dr = uci[3] - '1';
    return make_move_cmd(sc, sr, dc, dr, capture);
}

// Convenience: same but as a byte vector for the SimpleBLE
// write_request path which takes ByteArray (== std::string for
// SimpleBLE).
inline std::vector<uint8_t> make_move_cmd_bytes(int src_col, int src_row,
                                                int dst_col, int dst_row,
                                                bool capture) {
    std::string s = make_move_cmd(src_col, src_row, dst_col, dst_row, capture);
    return std::vector<uint8_t>(s.begin(), s.end());
}

// True iff the given device-name string looks like a Phantom board.
// Matched case-insensitively against advertising names — covers the
// two brand variants the firmware string table mentions.
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
