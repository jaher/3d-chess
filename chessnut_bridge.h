#pragma once

// Chessnut Move BLE bridge — desktop-only. Wraps SimpleBLE so the
// rest of the app doesn't have to think about BlueZ / IOBluetooth /
// Windows Runtime. PIMPL keeps the SimpleBLE headers out of every
// translation unit that pokes the bridge.
//
// Web build: stubbed; the Web Bluetooth path lives in
// web/chessnut_web.cpp under the same `app_chessnut_*` API that
// app_state.cpp calls into, so this header doesn't ship there.

#ifndef __EMSCRIPTEN__

#include "board_bridge.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class ChessnutBridge : public IBoardBridge {
public:
    ChessnutBridge();
    ~ChessnutBridge() override;

    ChessnutBridge(const ChessnutBridge&) = delete;
    ChessnutBridge& operator=(const ChessnutBridge&) = delete;

    // IBoardBridge overrides.
    bool start(StatusCallback on_status) override;
    void stop() override;
    bool running() const override;
    void connect_to_address(const std::string& address) override;
    void on_full_position_set(const std::string& fen) override;
    void on_move_played(const std::string& fen,
                        int src_col, int src_row,
                        int dst_col, int dst_row,
                        bool capture) override;
    void on_highlight_move(int src_col, int src_row,
                           int dst_col, int dst_row) override;
    const char* label() const override { return "Chessnut Move"; }

    // Chessnut-specific helpers retained for callers that need
    // direct access (the picker scan, the diagnostic probe).
    void request_connect();
    void start_scan();
    void send_fen(const std::string& fen, bool force);
    // Air-format LED frame (8-byte on/off bitmask). Kept for
    // potential Chessnut Air support — Move firmware silently
    // ignores it. New callers should prefer send_led_move_grid.
    void send_led_hex(const std::string& bitmask_hex);
    // Move-format LED frame (32-byte 4-bits-per-square RGB). Each
    // grid_color[row][col] is one of LED_COLOR_OFF/RED/GREEN/BLUE.
    // col 0 = h-file, col 7 = a-file; row 0 = rank 1, row 7 = rank 8.
    void send_led_move_grid(
        const std::array<std::array<uint8_t, 8>, 8>& grid_color);
    // Pulse the LED at (col, row) a few times with short on/off
    // gaps to draw the user's attention to that square. Used right
    // before a setMoveBoard so they can see where the motors are
    // about to push a piece.
    void blink_square(int col, int row);
    // Diagnostic — write 0x41 0x01 0x0B (getMovePieceState). Reply
    // arrives on a notify channel and is logged raw.
    void probe_piece_state();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif  // !__EMSCRIPTEN__
