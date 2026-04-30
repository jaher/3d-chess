#pragma once

// Chessnut Move BLE bridge — desktop-only. Spawns
// tools/chessnut_bridge.py as a subprocess and exchanges line-based
// commands with it (mirrors the Stockfish subprocess pattern in
// ai_player.cpp). The Python side does the actual BLE work via
// `bleak`; this header is the C++-side handle the chess app uses to
// drive the physical board.

#ifndef __EMSCRIPTEN__

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class ChessnutBridge {
public:
    // Status updates are delivered on a worker thread — the caller
    // is responsible for marshalling onto the GUI thread (e.g. via
    // g_idle_add). Possible status strings include "READY",
    // "CONNECTED", "DISCONNECTED", "ERROR <msg>", "FATAL <msg>".
    using StatusCallback = std::function<void(const std::string& status)>;

    ChessnutBridge() = default;
    ~ChessnutBridge();

    ChessnutBridge(const ChessnutBridge&) = delete;
    ChessnutBridge& operator=(const ChessnutBridge&) = delete;

    // Fork the helper. Returns false if fork/exec failed (no Python
    // interpreter, missing script, etc). On success the helper is
    // running but the BLE connection isn't open yet — call
    // request_connect() and wait for a CONNECTED status.
    bool start(StatusCallback on_status);

    // Send INIT to the helper. Status callback fires with
    // "CONNECTED <name>" on success or "ERROR ..." on failure.
    void request_connect();

    // Send a target FEN. force=true on game start / puzzle load so
    // the firmware always replans from current sensor state; false
    // for per-move updates so the board only acts when state
    // actually differs.
    void send_fen(const std::string& fen, bool force);

    // Send an 8-byte LED bitmask (one bit per square). Pass exactly
    // 16 hex chars.
    void send_led_hex(const std::string& bitmask_hex);

    // Cleanly shut the helper down (sends QUIT, closes pipes,
    // reaps). Safe to call repeatedly.
    void stop();

    bool running() const { return pid_ > 0; }

private:
    bool write_line(const std::string& line);
    void reader_loop();

    int  in_fd_  = -1;   // parent → child (stdin)
    int  out_fd_ = -1;   // child → parent (stdout)
    int  pid_    = -1;
    std::thread       reader_thread_;
    std::atomic<bool> stop_reader_{false};
    std::mutex        write_mu_;  // serialises writes to in_fd_
    StatusCallback    on_status_;
};

#endif  // !__EMSCRIPTEN__
