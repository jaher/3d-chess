#pragma once

// Pure-logic Chessnut Move wire-format encoder. Header-only so the
// desktop native impl (chessnut_bridge.cpp) and the web Web-
// Bluetooth impl (web/chessnut_web.cpp) share one source of truth.
// Mirrors the Python helper in tools/chessnut_bridge.py byte-for-byte.
//
// See docs/CHESSNUT.md for the full reverse-engineering
// notes — UUIDs, opcodes, and the firmware's piece-encoding map.

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace chessnut {

// ===========================================================================
// Wire-format opcodes (one source of truth for all drivers)
// ===========================================================================
// Outbound (host → firmware), routed via WRITE_UUID:
constexpr uint8_t OPCODE_LED            = 0x0A;  // Air: 8-byte on/off LED bitmask
constexpr uint8_t OPCODE_AUX_INIT       = 0x0B;  // post-subscribe handshake
constexpr uint8_t OPCODE_STREAM_ENABLE  = 0x21;  // enable board-state push (Air "init")
constexpr uint8_t OPCODE_LEGACY_INIT    = 0x27;  // legacy unbranded init — Move SKIPS this
constexpr uint8_t OPCODE_INFO_QUERY     = 0x41;  // 0x41 0x?? sub-commands
constexpr uint8_t OPCODE_SET_MOVE_BOARD = 0x42;  // motor command (target board)
constexpr uint8_t OPCODE_SET_MOVE_LED   = 0x43;  // Move: 32-byte RGB LED frame

// Inbound (firmware → host) opcodes seen on notify channels:
constexpr uint8_t OPCODE_FEN_DATA       = 0x01;  // 32-byte board-state push
constexpr uint8_t OPCODE_POWER_LEVEL    = 0x2A;  // battery / power frame
constexpr uint8_t OPCODE_INFO_REPLY     = 0x41;  // mirrors OPCODE_INFO_QUERY

// Sub-codes under 0x41 (OPCODE_INFO_QUERY) — selector byte at offset 2:
constexpr uint8_t INFO_GET_PIECE_STATE  = 0x0B;  // getMovePieceState

// Inbound FEN-data frame layout: [opcode, payload_len, board[32], trailer[4]]
//   ChessnutService.java:1011-1015 dispatches on (bytes[0]==0x01 && bytes[1]==0x24).
constexpr uint8_t FEN_DATA_PAYLOAD_LEN  = 0x24;  // = 36 bytes (32 board + 4 trailer)
constexpr size_t  FEN_DATA_TOTAL_BYTES  = 38;    // header(2) + payload(36)
constexpr size_t  FEN_DATA_HEX_CHARS    = FEN_DATA_TOTAL_BYTES * 2;  // = 76

// setMoveBoard outbound frame layout: [opcode, payload_len, board[32], force_byte]
constexpr uint8_t SET_MOVE_BOARD_PAYLOAD_LEN = 0x21;  // = 33 bytes (32 board + 1 force)
constexpr size_t  SET_MOVE_BOARD_TOTAL_BYTES = 35;

// Air LED outbound frame: [OPCODE_LED, LED_PAYLOAD_LEN (=8), bitmask[8]].
// One bit per square; LEDs are monochrome.
constexpr uint8_t LED_PAYLOAD_LEN       = 0x08;

// Move LED outbound frame: [OPCODE_SET_MOVE_LED, SET_MOVE_LED_PAYLOAD_LEN
// (=32), grid[32]]. 4 bits per square in the same pair-reversed packing
// as the setMoveBoard frame. Each nibble is a colour code:
//   0 = off
//   1 = red
//   2 = green
//   3 = blue (also the fall-through when the Android app passes
//            non-zero ledSwitches but r==0 && g==0)
constexpr uint8_t SET_MOVE_LED_PAYLOAD_LEN = 0x20;  // = 32 bytes
constexpr size_t  SET_MOVE_LED_TOTAL_BYTES = 34;
constexpr uint8_t LED_COLOR_OFF   = 0;
constexpr uint8_t LED_COLOR_RED   = 1;
constexpr uint8_t LED_COLOR_GREEN = 2;
constexpr uint8_t LED_COLOR_BLUE  = 3;

// ---------------------------------------------------------------------------
// Pre-built command frames used by the post-subscribe handshake and the
// diagnostic probe. All three drivers (chessnut_bridge.cpp,
// web/chessnut_web.cpp, tools/chessnut_bridge.py) send the same bytes —
// the C++ side reuses these constants directly; the JS / Python sides
// mirror them as their own literals, kept in sync by hand.
//
// Source: ChessnutBLEDevice.java:339, 342 (handshake) + ChessnutService.java
// :353-356 (getMovePieceState).
// ---------------------------------------------------------------------------
constexpr std::array<uint8_t, 3> CMD_STREAM_ENABLE = {
    OPCODE_STREAM_ENABLE, 0x01, 0x00,
};
constexpr std::array<uint8_t, 6> CMD_AUX_INIT = {
    OPCODE_AUX_INIT, 0x04, 0x03, 0xE8, 0x00, 0xC8,
};
constexpr std::array<uint8_t, 3> CMD_GET_PIECE_STATE = {
    OPCODE_INFO_QUERY, 0x01, INFO_GET_PIECE_STATE,
};

// PIECEMAP from ChessnutService.java:78-90. Lowercase = black,
// uppercase = white, space = empty.
inline uint8_t piece_to_nibble(char c) {
    switch (c) {
    case ' ': return 0;
    case 'q': return 1;
    case 'k': return 2;
    case 'b': return 3;
    case 'p': return 4;
    case 'n': return 5;
    case 'R': return 6;
    case 'P': return 7;
    case 'r': return 8;
    case 'B': return 9;
    case 'N': return 10;
    case 'Q': return 11;
    case 'K': return 12;
    default:  return 0;
    }
}

// Inverse of piece_to_nibble — used by the inbound sensor-frame
// decoder to rebuild a piece-placement grid from what the board
// reports over the 8262 notify channel.
inline char nibble_to_piece(uint8_t n) {
    static const char map[13] = {
        ' ', 'q', 'k', 'b', 'p', 'n',
        'R', 'P', 'r', 'B', 'N', 'Q', 'K',
    };
    return n < 13 ? map[n] : ' ';
}

// Decode the 32-byte board frame into an 8x8 grid of piece chars,
// indexed by [row][col] using the project's internal coordinate
// system (row 7 = rank 8, col 7 = a-file). ' ' = empty.
//
// This is the inverse of fen_to_board_bytes — same pair-reverse
// ordering, same nibble map.
inline std::array<std::array<char, 8>, 8>
board_bytes_to_grid(const std::array<uint8_t, 32>& bytes) {
    std::array<std::array<char, 8>, 8> grid{};
    for (auto& row : grid) row.fill(' ');
    for (int i = 0; i < 8; i++) {       // FEN rank index (0=rank8)
        int internal_row = 7 - i;
        for (int i2 = 0; i2 < 4; i2++) {  // file pair
            uint8_t b = bytes[(i * 4) + (3 - i2)];
            uint8_t hi = static_cast<uint8_t>((b >> 4) & 0x0F);
            uint8_t lo = static_cast<uint8_t>(b & 0x0F);
            int col_hi = 7 - (2 * i2);
            int col_lo = 7 - (2 * i2 + 1);
            grid[internal_row][col_hi] = nibble_to_piece(hi);
            grid[internal_row][col_lo] = nibble_to_piece(lo);
        }
    }
    return grid;
}

// Encode the placement portion of a FEN into the 32-byte 4-bits-per-
// square format the Move firmware expects. Pair index `i2` runs 0..3
// but is stored at offset `(3 - i2)` within each row, so the h-pair
// lands at offset 0 and the a-pair at offset 3 — see the original
// Java loop in ChessnutService.java:330-350.
inline std::array<uint8_t, 32> fen_to_board_bytes(const std::string& fen) {
    std::array<uint8_t, 32> board{};
    std::string placement = fen.substr(0, fen.find(' '));
    std::vector<std::string> rows;
    std::string acc;
    for (char c : placement) {
        if (c == '/') { rows.push_back(acc); acc.clear(); }
        else acc.push_back(c);
    }
    rows.push_back(acc);
    if (rows.size() != 8) {
        throw std::runtime_error("fen has wrong rank count");
    }
    for (int i = 0; i < 8; i++) {
        std::string expanded;
        for (char c : rows[i]) {
            if (c >= '1' && c <= '8') expanded.append(c - '0', ' ');
            else                      expanded.push_back(c);
        }
        if (expanded.size() != 8) {
            throw std::runtime_error("fen row not 8 squares wide");
        }
        for (int i2 = 0; i2 < 4; i2++) {
            uint8_t hi = piece_to_nibble(expanded[i2 * 2]);
            uint8_t lo = piece_to_nibble(expanded[i2 * 2 + 1]);
            board[(i * 4) + (3 - i2)] = static_cast<uint8_t>((hi << 4) | lo);
        }
    }
    return board;
}

// Build the full 35-byte setMoveBoard frame:
//   [0]   OPCODE_SET_MOVE_BOARD       opcode
//   [1]   SET_MOVE_BOARD_PAYLOAD_LEN  payload length (=33)
//   [2..33] board state                4 bits per square
//   [34]  0 if force else 1            0 = always replan from sensor state
inline std::vector<uint8_t> make_set_move_board(const std::string& fen,
                                                bool force) {
    auto board = fen_to_board_bytes(fen);
    std::vector<uint8_t> frame;
    frame.reserve(SET_MOVE_BOARD_TOTAL_BYTES);
    frame.push_back(OPCODE_SET_MOVE_BOARD);
    frame.push_back(SET_MOVE_BOARD_PAYLOAD_LEN);
    for (uint8_t b : board) frame.push_back(b);
    frame.push_back(force ? 0u : 1u);
    return frame;
}

// Air LED frame: [OPCODE_LED, LED_PAYLOAD_LEN (=8), bitmask[8]] from a
// 16-hex-char bitmask string. One bit per square; monochrome.
inline std::vector<uint8_t> make_led_frame(const std::string& bitmask_hex) {
    if (bitmask_hex.size() != 16)
        throw std::runtime_error("LED bitmask must be 16 hex chars");
    std::vector<uint8_t> frame;
    frame.reserve(2 + LED_PAYLOAD_LEN);
    frame.push_back(OPCODE_LED);
    frame.push_back(LED_PAYLOAD_LEN);
    for (size_t i = 0; i < 16; i += 2) {
        unsigned v = 0;
        std::sscanf(bitmask_hex.substr(i, 2).c_str(), "%x", &v);
        frame.push_back(static_cast<uint8_t>(v));
    }
    return frame;
}

// Move LED frame: [OPCODE_SET_MOVE_LED, SET_MOVE_LED_PAYLOAD_LEN (=32),
// grid[32]]. grid_color[row][col] is a 4-bit colour code (0..3); see
// LED_COLOR_* constants. Packing matches setMoveBoard: 8 ranks × 4
// pairs, pair-reversed within each rank (h-pair at offset 0, a-pair at
// offset 3 of each rank's 4-byte slice). Rank order: i=0 → rank 8
// (algebraic), i.e. our internal row 7. col 0 = h-file, col 7 = a-file
// (project's internal coords).
inline std::vector<uint8_t> make_led_move_frame(
    const std::array<std::array<uint8_t, 8>, 8>& grid_color) {
    std::vector<uint8_t> frame(2 + SET_MOVE_LED_PAYLOAD_LEN, 0);
    frame[0] = OPCODE_SET_MOVE_LED;
    frame[1] = SET_MOVE_LED_PAYLOAD_LEN;
    for (int i = 0; i < 8; i++) {
        int internal_row = 7 - i;  // i=0 is rank 8 = our row 7
        for (int i2 = 0; i2 < 4; i2++) {
            int col_hi = 7 - (2 * i2);
            int col_lo = 7 - (2 * i2 + 1);
            uint8_t hi = grid_color[internal_row][col_hi] & 0x0F;
            uint8_t lo = grid_color[internal_row][col_lo] & 0x0F;
            frame[2 + (i * 4) + (3 - i2)] =
                static_cast<uint8_t>((hi << 4) | lo);
        }
    }
    return frame;
}

}  // namespace chessnut
