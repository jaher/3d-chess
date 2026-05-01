#pragma once

// Chessnut Move BLE bridge — desktop-only. Public interface for
// the chess app. Implementation is a private PIMPL — selected at
// construction time:
//
//   * Native (default): SimpleBLE in-process. Talks to BlueZ via
//     D-Bus on Linux, IOBluetooth on macOS, Windows Runtime on
//     Windows. No subprocess, no Python dependency.
//   * Python (debug): forks tools/chessnut_bridge.py. Requires
//     python3 + bleak. Set CHESS_CHESSNUT_USE_PYTHON=1 to choose
//     this — useful for protocol experimentation, since the
//     Python helper logs every notification and is easy to drive
//     by hand.

#ifndef __EMSCRIPTEN__

#include <functional>
#include <memory>
#include <string>

class IChessnutBridgeImpl;  // see chessnut_bridge_impl.h

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
    bool running() const;

private:
    std::unique_ptr<IChessnutBridgeImpl> impl_;
};

#endif  // !__EMSCRIPTEN__
