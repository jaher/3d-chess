// Chessnut Move BLE bridge — public dispatcher. The actual BLE work
// lives in chessnut_bridge_native.cpp (default) or
// chessnut_bridge_python.cpp (CHESS_CHESSNUT_USE_PYTHON=1).

#ifndef __EMSCRIPTEN__

#include "chessnut_bridge.h"
#include "chessnut_bridge_impl.h"

#include <cstdlib>

namespace {
bool want_python_impl() {
    const char* env = std::getenv("CHESS_CHESSNUT_USE_PYTHON");
    return env && *env && std::string(env) != "0";
}
}  // namespace

ChessnutBridge::ChessnutBridge() {
    impl_ = want_python_impl() ? make_chessnut_python_impl()
                               : make_chessnut_native_impl();
}

ChessnutBridge::~ChessnutBridge() = default;

bool ChessnutBridge::start(StatusCallback on_status) {
    return impl_ ? impl_->start(std::move(on_status)) : false;
}

void ChessnutBridge::stop()             { if (impl_) impl_->stop(); }
void ChessnutBridge::request_connect()  { if (impl_) impl_->request_connect(); }
void ChessnutBridge::start_scan()       { if (impl_) impl_->start_scan(); }
void ChessnutBridge::connect_to_address(const std::string& address) {
    if (impl_) impl_->connect_to_address(address);
}

void ChessnutBridge::send_fen(const std::string& fen, bool force) {
    if (impl_) impl_->send_fen(fen, force);
}
void ChessnutBridge::send_led_hex(const std::string& hex) {
    if (impl_) impl_->send_led_hex(hex);
}
void ChessnutBridge::probe_piece_state() {
    if (impl_) impl_->probe_piece_state();
}
bool ChessnutBridge::running() const {
    return impl_ && impl_->running();
}

#endif  // !__EMSCRIPTEN__
