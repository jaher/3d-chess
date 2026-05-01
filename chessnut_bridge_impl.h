#pragma once

// Internal interface for ChessnutBridge implementations. Two
// concrete impls live in this codebase:
//
//   chessnut_bridge_native.cpp  — SimpleBLE in-process (default)
//   chessnut_bridge_python.cpp  — fork+exec tools/chessnut_bridge.py
//                                  (debugging fallback, set via the
//                                  CHESS_CHESSNUT_USE_PYTHON env var)
//
// The public ChessnutBridge in chessnut_bridge.h owns a
// unique_ptr<IChessnutBridgeImpl> and forwards every call.

#ifndef __EMSCRIPTEN__

#include <functional>
#include <memory>
#include <string>

class IChessnutBridgeImpl {
public:
    using StatusCallback = std::function<void(const std::string&)>;
    virtual ~IChessnutBridgeImpl() = default;
    virtual bool start(StatusCallback on_status) = 0;
    virtual void stop() = 0;
    virtual void request_connect() = 0;

    // Scan all advertising peripherals and emit one
    // "DEVICE <mac> <name>" status per discovered device, followed
    // by a single "SCAN_COMPLETE" once scanning finishes. Used by
    // the in-app picker so the user can choose which board to bind
    // to.
    virtual void start_scan() = 0;

    // Connect to a specific MAC, skipping the discovery phase. Used
    // by the picker after the user clicks a row.
    virtual void connect_to_address(const std::string& address) = 0;

    virtual void send_fen(const std::string& fen, bool force) = 0;
    virtual void send_led_hex(const std::string& bitmask_hex) = 0;
    virtual bool running() const = 0;
};

// Factories — defined in the matching .cpp files. Each returns a
// fresh impl; failure to construct is signalled by start() returning
// false rather than by the factory itself.
std::unique_ptr<IChessnutBridgeImpl> make_chessnut_native_impl();
std::unique_ptr<IChessnutBridgeImpl> make_chessnut_python_impl();

#endif  // !__EMSCRIPTEN__
