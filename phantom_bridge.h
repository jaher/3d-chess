#pragma once

// Phantom Chessboard BLE bridge — desktop-only. Sibling to
// ChessnutBridge: same threading model (SimpleBLE worker thread,
// command queue, status callback), but different protocol.
// Implements the shared IBoardBridge interface so the rest of the
// app can dispatch per-move events polymorphically without
// branching on which protocol the connected device speaks.
//
// Web build: stubbed; the Web Bluetooth path lives in
// web/chessnut_web.cpp behind the same `app_chessnut_*` API that
// app_state.cpp calls into, so this header doesn't ship there.
//
// See docs/PHANTOM.md for the protocol notes and phantom_encode.h for
// the wire-format helpers.

#ifndef __EMSCRIPTEN__

#include "board_bridge.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class PhantomBridge : public IBoardBridge {
public:
    PhantomBridge();
    ~PhantomBridge() override;

    PhantomBridge(const PhantomBridge&) = delete;
    PhantomBridge& operator=(const PhantomBridge&) = delete;

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
    // on_highlight_move uses the IBoardBridge default no-op —
    // Phantom has no per-move LED API on verified channels.
    const char* label() const override { return "Phantom"; }

    // Phantom-specific raw writes (v0.3.0 protocol). Coordinates are
    // file/rank indices (0 = a-file / rank 1) — NOT the app's internal
    // board columns. `piece` is the moved piece's FEN letter (advisory;
    // 'E' if unknown). Exposed for debugging / manual driving.
    void send_move(int src_file, int src_row, int dst_file, int dst_row,
                   bool capture, char piece = 'E');

    // Put the board into play mode (write "2" to the mode characteristic)
    // so it starts reporting detected sensor moves. Done automatically on
    // connect; exposed for re-sync / debugging.
    void enter_play_mode();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif  // !__EMSCRIPTEN__
