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

#include <functional>
#include <memory>
#include <string>

class ChessnutBridge {
public:
    using StatusCallback = std::function<void(const std::string& status)>;

    ChessnutBridge();
    ~ChessnutBridge();

    ChessnutBridge(const ChessnutBridge&) = delete;
    ChessnutBridge& operator=(const ChessnutBridge&) = delete;

    bool start(StatusCallback on_status);
    void stop();
    void request_connect();
    void start_scan();
    void connect_to_address(const std::string& address);
    void send_fen(const std::string& fen, bool force);
    void send_led_hex(const std::string& bitmask_hex);
    // Diagnostic — write 0x41 0x01 0x0B (getMovePieceState). Reply
    // arrives on a notify channel and is logged raw.
    void probe_piece_state();
    bool running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif  // !__EMSCRIPTEN__
