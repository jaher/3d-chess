#pragma once

// Pure-logic Chessnut Move wire-format encoder. Header-only so the
// desktop native impl (chessnut_bridge_native.cpp) and the web Web-
// Bluetooth impl (web/chessnut_web.cpp) share one source of truth.
// Mirrors the Python helper in tools/chessnut_bridge.py byte-for-byte.
//
// See ~/chessnutapp/PROTOCOL.md for the full reverse-engineering
// notes — UUIDs, opcodes, and the firmware's piece-encoding map.

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace chessnut {

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

// Build the full 35-byte 0x42 setMoveBoard frame:
//   [0]   0x42                  opcode
//   [1]   0x21 (33)             payload length
//   [2..33] board state          4 bits per square
//   [34]  0 if force else 1     0 = always replan from sensor state
inline std::vector<uint8_t> make_set_move_board(const std::string& fen,
                                                bool force) {
    auto board = fen_to_board_bytes(fen);
    std::vector<uint8_t> frame;
    frame.reserve(35);
    frame.push_back(0x42);
    frame.push_back(0x21);
    for (uint8_t b : board) frame.push_back(b);
    frame.push_back(force ? 0u : 1u);
    return frame;
}

// 0x0A length-8 LED frame from a 16-hex-char bitmask string.
inline std::vector<uint8_t> make_led_frame(const std::string& bitmask_hex) {
    if (bitmask_hex.size() != 16)
        throw std::runtime_error("LED bitmask must be 16 hex chars");
    std::vector<uint8_t> frame;
    frame.reserve(10);
    frame.push_back(0x0A);
    frame.push_back(0x08);
    for (size_t i = 0; i < 16; i += 2) {
        unsigned v = 0;
        std::sscanf(bitmask_hex.substr(i, 2).c_str(), "%x", &v);
        frame.push_back(static_cast<uint8_t>(v));
    }
    return frame;
}

}  // namespace chessnut
