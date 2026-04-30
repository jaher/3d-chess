// Chessnut Move BLE bridge — native (in-process) implementation
// using SimpleBLE. Default impl since SimpleBLE removes the
// python3 + bleak runtime dependency. The Python subprocess
// implementation in chessnut_bridge_python.cpp is kept as an
// escape hatch (set CHESS_CHESSNUT_USE_PYTHON=1).

#ifndef __EMSCRIPTEN__

#include "chessnut_bridge_impl.h"
#include "chessnut_encode.h"  // shared with web/chessnut_web.cpp

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <simpleble/SimpleBLE.h>
#include <simpleble/Adapter.h>
#include <simpleble/Peripheral.h>

namespace {

// ---------------------------------------------------------------------------
// GATT — see PROTOCOL.md (notes/chessnutapp). Same UUID family as the
// Chessnut Air community-RE'd protocol; Move adds the 8261/8271 pair.
// ---------------------------------------------------------------------------
constexpr const char* SUFFIX = "-2877-41c3-b46e-cf057c562023";
const std::string SVC_UUID         = std::string("1b7e8260") + SUFFIX;
// Listen on every notify channel — Air-compatible (8262, 8273) and
// Move-only (8261, 8271). Subscribing to channels the firmware
// doesn't expose just throws on attach; we tolerate that.
const std::string NOTIFY_UUIDS[] = {
    std::string("1b7e8261") + SUFFIX,
    std::string("1b7e8262") + SUFFIX,
    std::string("1b7e8271") + SUFFIX,
    std::string("1b7e8273") + SUFFIX,
};
const std::string WRITE_UUID = std::string("1b7e8272") + SUFFIX;

SimpleBLE::ByteArray to_byte_array(const std::vector<uint8_t>& bytes) {
    SimpleBLE::ByteArray out;
    out.reserve(bytes.size());
    for (uint8_t b : bytes) out.push_back(static_cast<uint8_t>(b));
    return out;
}

SimpleBLE::ByteArray frame_from_bytes(std::initializer_list<uint8_t> bs) {
    SimpleBLE::ByteArray out;
    for (uint8_t b : bs) out.push_back(static_cast<uint8_t>(b));
    return out;
}

// ---------------------------------------------------------------------------
// Bridge — runs all SimpleBLE work on a dedicated worker thread.
// SimpleBLE's API is mostly synchronous (scan_for blocks, connect
// blocks, write_command blocks); driving it from a worker keeps the
// GTK main thread unblocked.
// ---------------------------------------------------------------------------
struct Command {
    enum Kind { CONNECT, FEN, FEN_FORCE, LED, QUIT };
    Kind        kind;
    std::string arg;     // FEN string or LED hex; empty for others
};

class NativeImpl : public IChessnutBridgeImpl {
public:
    ~NativeImpl() override { stop(); }

    bool start(StatusCallback on_status) override {
        if (running_.load()) return true;
        on_status_ = std::move(on_status);
        running_.store(true);
        worker_ = std::thread([this] { worker_loop(); });
        emit("READY");
        return true;
    }

    void stop() override {
        if (!running_.exchange(false)) return;
        enqueue({Command::QUIT, ""});
        if (worker_.joinable()) worker_.join();
        on_status_ = nullptr;
    }

    void request_connect() override     { enqueue({Command::CONNECT,   ""}); }
    void send_fen(const std::string& fen, bool force) override {
        enqueue({force ? Command::FEN_FORCE : Command::FEN, fen});
    }
    void send_led_hex(const std::string& hex) override {
        enqueue({Command::LED, hex});
    }
    bool running() const override { return running_.load(); }

private:
    void enqueue(Command c) {
        {
            std::lock_guard<std::mutex> lk(q_mu_);
            queue_.push_back(std::move(c));
        }
        q_cv_.notify_one();
    }

    Command dequeue() {
        std::unique_lock<std::mutex> lk(q_mu_);
        q_cv_.wait(lk, [this] { return !queue_.empty(); });
        Command c = std::move(queue_.front());
        queue_.pop_front();
        return c;
    }

    void emit(const std::string& s) {
        if (on_status_) on_status_(s);
    }

    void worker_loop() {
        while (true) {
            Command c = dequeue();
            try {
                switch (c.kind) {
                case Command::QUIT:
                    if (peripheral_initialised_ && peripheral_.is_connected()) {
                        try { peripheral_.disconnect(); } catch (...) {}
                    }
                    return;
                case Command::CONNECT: do_connect(); break;
                case Command::FEN:
                    do_write(to_byte_array(chessnut::make_set_move_board(c.arg, false)), "FEN");
                    break;
                case Command::FEN_FORCE:
                    do_write(to_byte_array(chessnut::make_set_move_board(c.arg, true)), "FEN_FORCE");
                    break;
                case Command::LED:
                    do_write(to_byte_array(chessnut::make_led_frame(c.arg)), "LED");
                    break;
                }
            } catch (const std::exception& e) {
                emit(std::string("ERROR ") + e.what());
            } catch (...) {
                emit("ERROR unknown failure");
            }
        }
    }

    void do_connect() {
        if (peripheral_initialised_ && peripheral_.is_connected()) {
            emit("CONNECTED " + connected_name_);
            return;
        }
        if (!SimpleBLE::Adapter::bluetooth_enabled()) {
            emit("ERROR Bluetooth disabled or unavailable");
            return;
        }
        auto adapters = SimpleBLE::Adapter::get_adapters();
        if (adapters.empty()) { emit("ERROR no BLE adapter"); return; }
        SimpleBLE::Adapter adapter = adapters.front();

        // Scan for "Chessnut Move" with an 8-second window.
        SimpleBLE::Peripheral target;
        bool found = false;
        adapter.set_callback_on_scan_found(
            [&](SimpleBLE::Peripheral p) {
                if (found) return;
                std::string name;
                try { name = p.identifier(); } catch (...) { return; }
                if (name.rfind("Chessnut Move", 0) == 0 ||
                    name == "Chessnut Move") {
                    target = p;
                    found  = true;
                }
            });
        adapter.scan_for(8000);
        if (!found) {
            emit("ERROR no Chessnut Move device found");
            return;
        }
        connected_name_ = target.identifier();
        target.set_callback_on_disconnected([this] {
            emit("DISCONNECTED");
        });
        target.connect();
        peripheral_ = target;
        peripheral_initialised_ = true;

        // Subscribe to every notify UUID — Air firmware won't have
        // 8261/8271, Move firmware exposes all four. Failures on a
        // missing characteristic are non-fatal.
        for (const auto& uuid : NOTIFY_UUIDS) {
            try {
                peripheral_.notify(SVC_UUID, uuid,
                    [this, uuid](SimpleBLE::ByteArray data) {
                        std::ostringstream oss;
                        oss << "NOTIFY " << uuid << ' ';
                        for (unsigned char b : data) {
                            char buf[3];
                            std::snprintf(buf, sizeof(buf), "%02x", b);
                            oss << buf;
                        }
                        emit(oss.str());
                    });
            } catch (...) {
                // not present on this firmware — silent skip
            }
        }

        // Two-frame Move handshake — see ChessnutBLEDevice.java:342, 350.
        try {
            peripheral_.write_command(
                SVC_UUID, WRITE_UUID,
                frame_from_bytes({0x0B, 0x04, 0x03, 0xE8, 0x00, 0xC8}));
            peripheral_.write_command(
                SVC_UUID, WRITE_UUID,
                frame_from_bytes({0x27, 0x01, 0x00}));
        } catch (const std::exception& e) {
            emit(std::string("ERROR handshake failed: ") + e.what());
            return;
        }
        emit("CONNECTED " + connected_name_);
    }

    void do_write(const SimpleBLE::ByteArray& frame, const char* tag) {
        if (!peripheral_initialised_ || !peripheral_.is_connected()) {
            emit("ERROR not connected");
            return;
        }
        try {
            peripheral_.write_command(SVC_UUID, WRITE_UUID, frame);
        } catch (const std::exception& e) {
            emit(std::string("ERROR write failed: ") + e.what());
            return;
        }
        emit(std::string("ACK ") + tag);
    }

    StatusCallback              on_status_;
    std::atomic<bool>           running_{false};
    std::thread                 worker_;
    std::mutex                  q_mu_;
    std::condition_variable     q_cv_;
    std::deque<Command>         queue_;
    SimpleBLE::Peripheral       peripheral_;
    bool                        peripheral_initialised_ = false;
    std::string                 connected_name_;
};

}  // namespace

std::unique_ptr<IChessnutBridgeImpl> make_chessnut_native_impl() {
    return std::make_unique<NativeImpl>();
}

#endif  // !__EMSCRIPTEN__
