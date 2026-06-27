// Phantom Chessboard BLE bridge — SimpleBLE-backed (firmware v0.3.0). Siblings
// the Chessnut Move bridge in chessnut_bridge.cpp. Different protocol (an
// opcode-framed game characteristic, see phantom_encode.h) but the threading
// model is identical: a worker thread drains a command queue, all SimpleBLE
// calls happen there, status is surfaced through a callback registered by
// app_state.cpp.
//
// Protocol (decompiled from the official app, cross-checked on a real board):
//   * On connect, after GATT discovery, we write the play-mode digit "2" to
//     the mode characteristic so the board starts reporting sensor moves.
//   * We subscribe to every notify-capable characteristic and forward frames
//     raw as "NOTIFY <uuid> <hex>"; app_state.cpp parses the GAME-characteristic
//     0x06 "detected move" frames into digital moves.
//   * A move is pushed to the board as two writes to the GAME characteristic:
//     a side frame ([0x0a]+"2") then a movement frame ([0x02]+"M e2-e4 P").
// See docs/PHANTOM.md and phantom_encode.h for the wire format.

#ifndef __EMSCRIPTEN__

#include "phantom_bridge.h"
#include "phantom_encode.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// SimpleBLE's Config.h defines `static void reset_all()` inline,
// which trips -Wunused-function when this TU doesn't reference it.
// Scope the suppression to the third-party header block.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include <simpleble/Adapter.h>
#include <simpleble/Characteristic.h>
#include <simpleble/Peripheral.h>
#include <simpleble/Service.h>
#include <simpleble/SimpleBLE.h>
#pragma GCC diagnostic pop

namespace {

SimpleBLE::ByteArray to_byte_array(const std::vector<uint8_t>& v) {
    SimpleBLE::ByteArray out;
    out.reserve(v.size());
    for (uint8_t b : v) out.push_back(static_cast<uint8_t>(b));
    return out;
}

struct Command {
    enum Kind { CONNECT_TO, WRITE, QUIT };
    Kind kind;
    std::string addr;                 // CONNECT_TO
    std::string char_uuid;            // WRITE: target characteristic
    std::vector<uint8_t> payload;     // WRITE: bytes to write
};

}  // namespace

struct PhantomBridge::Impl {
    ~Impl() { stop(); }

    bool start(StatusCallback on_status) {
        if (running_.load()) return true;
        on_status_ = std::move(on_status);
        running_.store(true);
        worker_ = std::thread([this] { worker_loop(); });
        emit("READY");
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        enqueue({Command::QUIT, "", "", {}});
        if (worker_.joinable()) worker_.join();
        on_status_ = nullptr;
    }

    void connect_to_address(const std::string& addr) {
        enqueue({Command::CONNECT_TO, addr, "", {}});
    }

    // Drive a move on the robot: side frame then movement frame, both to the
    // GAME characteristic. Coordinates are file/rank indices (0 = a-file).
    void send_move(int src_file, int src_row, int dst_file, int dst_row,
                   bool capture, char piece) {
        auto frames = phantom::make_move_frames(src_file, src_row,
                                                dst_file, dst_row,
                                                capture, piece);
        enqueue({Command::WRITE, "", phantom::GAME_UUID, frames[0]});
        enqueue({Command::WRITE, "", phantom::GAME_UUID, frames[1]});
    }

    void enter_play_mode() {
        std::vector<uint8_t> payload(phantom::MODE_PLAY,
                                     phantom::MODE_PLAY + 1);  // "2"
        enqueue({Command::WRITE, "", phantom::MODE_UUID, payload});
    }

    bool running() const { return running_.load(); }

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
        std::fprintf(stderr, "[phantom/native] %s\n", s.c_str());
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
                case Command::CONNECT_TO:
                    do_connect(c.addr);
                    break;
                case Command::WRITE:
                    do_write(c.char_uuid, c.payload);
                    break;
                }
            } catch (const std::exception& e) {
                emit(std::string("ERROR ") + e.what());
            } catch (...) {
                emit("ERROR unknown failure");
            }
        }
    }

    void teardown_peripheral() {
        if (peripheral_initialised_) {
            try {
                if (peripheral_.is_connected()) peripheral_.disconnect();
            } catch (...) { /* best-effort */ }
        }
        peripheral_initialised_ = false;
        char_to_service_.clear();
    }

    void do_connect(const std::string& addr) {
        if (peripheral_initialised_ && peripheral_.is_connected() &&
            !char_to_service_.empty()) {
            emit("CONNECTED " + connected_name_);
            return;
        }
        if (peripheral_initialised_) teardown_peripheral();
        if (!SimpleBLE::Adapter::bluetooth_enabled()) {
            emit("ERROR Bluetooth disabled or unavailable");
            return;
        }
        auto adapters = SimpleBLE::Adapter::get_adapters();
        if (adapters.empty()) { emit("ERROR no BLE adapter"); return; }
        SimpleBLE::Adapter adapter = adapters.front();

        SimpleBLE::Peripheral target;
        bool found = false;
        adapter.set_callback_on_scan_found(
            [&](SimpleBLE::Peripheral p) {
                if (found) return;
                try {
                    if (p.address() != addr) return;
                } catch (...) { return; }
                target = p;
                found  = true;
            });
        adapter.scan_for(4000);
        if (!found) {
            emit("ERROR Phantom device " + addr + " not found");
            return;
        }
        connected_name_ = target.identifier();
        target.set_callback_on_disconnected([this] {
            emit("DISCONNECTED");
        });
        target.connect();
        peripheral_ = target;
        peripheral_initialised_ = true;

        // Discover the actual service tree. BlueZ sometimes reports
        // connect() complete before its GATT view is fully resolved
        // — same retry pattern as the Chessnut bridge.
        char_to_service_.clear();
        std::vector<std::pair<std::string, std::string>> notify_chars;
        std::vector<SimpleBLE::Service> services;
        std::string last_error;
        constexpr int kAttempts = 4;
        for (int attempt = 0; attempt < kAttempts; attempt++) {
            try {
                services = peripheral_.services();
                last_error.clear();
                if (!services.empty()) break;
                last_error = "no services returned";
            } catch (const std::exception& e) {
                last_error = e.what();
            }
            std::fprintf(stderr,
                "[phantom/native] services() attempt %d/%d failed: %s\n",
                attempt + 1, kAttempts, last_error.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (!last_error.empty()) {
            emit(std::string("ERROR service discovery: ") + last_error);
            teardown_peripheral();
            return;
        }
        std::fprintf(stderr,
            "[phantom/native] services discovered: %zu\n",
            services.size());
        for (auto& s : services) {
            for (auto& c : s.characteristics()) {
                char_to_service_[c.uuid()] = s.uuid();
                bool n = false;
                try { n = c.can_notify() || c.can_indicate(); }
                catch (...) { n = false; }
                std::fprintf(stderr,
                    "[phantom/native]     char %s%s\n",
                    c.uuid().c_str(), n ? " [NOTIFY]" : "");
                if (n) notify_chars.emplace_back(s.uuid(), c.uuid());
            }
            std::fprintf(stderr,
                "[phantom/native]   service %s (%zu chars)\n",
                s.uuid().c_str(), s.characteristics().size());
        }
        // The v0.3.0 protocol runs over the GAME characteristic. If it isn't
        // present this is almost certainly an older-firmware board (whose
        // motor-cmd lived on 7b204548-30c3…) that we no longer drive.
        if (char_to_service_.find(phantom::GAME_UUID)
            == char_to_service_.end()) {
            std::string msg = std::string("ERROR game characteristic ")
                 + phantom::GAME_UUID + " not exposed by this peripheral";
            if (char_to_service_.find(phantom::LEGACY_MOVE_CMD_UUID)
                != char_to_service_.end()) {
                msg += " (looks like pre-0.3.0 firmware — unsupported)";
            }
            emit(msg);
            teardown_peripheral();
            return;
        }

        // Subscribe to every notify-capable characteristic. The GAME
        // characteristic's 0x06 frames carry detected moves; the rest are
        // logged raw for diagnostics. app_state.cpp does the parsing.
        for (const auto& [svc_uuid, char_uuid] : notify_chars) {
            try {
                peripheral_.notify(svc_uuid, char_uuid,
                    [this, char_uuid](SimpleBLE::ByteArray data) {
                        std::ostringstream oss;
                        oss << "NOTIFY " << char_uuid << ' ';
                        for (unsigned char b : data) {
                            char buf[3];
                            std::snprintf(buf, sizeof(buf), "%02x", b);
                            oss << buf;
                        }
                        emit(oss.str());
                    });
            } catch (...) {
                // notify subscribe refused — non-fatal.
            }
        }

        try {
            uint16_t mtu = peripheral_.mtu();
            std::fprintf(stderr,
                "[phantom/native] negotiated MTU = %u\n",
                static_cast<unsigned>(mtu));
        } catch (...) { /* MTU getter is best-effort */ }

        // Enter play mode so the board begins pushing detected moves.
        do_write(phantom::MODE_UUID,
                 std::vector<uint8_t>(phantom::MODE_PLAY,
                                      phantom::MODE_PLAY + 1));

        emit("CONNECTED " + connected_name_);
    }

    void do_write(const std::string& char_uuid,
                  const std::vector<uint8_t>& payload) {
        if (!peripheral_initialised_ || !peripheral_.is_connected()) {
            emit("ERROR not connected");
            return;
        }
        auto it = char_to_service_.find(char_uuid);
        if (it == char_to_service_.end()) {
            emit("ERROR characteristic " + char_uuid + " not discovered");
            return;
        }
        std::ostringstream hex;
        for (uint8_t b : payload) {
            char buf[3];
            std::snprintf(buf, sizeof(buf), "%02x", b);
            hex << buf;
        }
        std::fprintf(stderr,
            "[phantom/native] write %s len=%zu bytes=%s\n",
            char_uuid.c_str(), payload.size(), hex.str().c_str());
        try {
            peripheral_.write_request(it->second, char_uuid,
                                      to_byte_array(payload));
        } catch (const std::exception& e) {
            emit(std::string("ERROR write failed: ") + e.what());
            return;
        }
        emit("ACK WRITE");
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
    std::map<std::string, std::string> char_to_service_;
};

PhantomBridge::PhantomBridge() : impl_(std::make_unique<Impl>()) {}
PhantomBridge::~PhantomBridge() = default;

bool PhantomBridge::start(StatusCallback on_status) {
    return impl_->start(std::move(on_status));
}
void PhantomBridge::stop() { impl_->stop(); }
void PhantomBridge::connect_to_address(const std::string& addr) {
    impl_->connect_to_address(addr);
}
void PhantomBridge::send_move(int src_file, int src_row,
                              int dst_file, int dst_row, bool capture,
                              char piece) {
    impl_->send_move(src_file, src_row, dst_file, dst_row, capture, piece);
}
void PhantomBridge::enter_play_mode() { impl_->enter_play_mode(); }
bool PhantomBridge::running() const { return impl_->running(); }

// ---------------------------------------------------------------------------
// IBoardBridge polymorphic overrides.
// ---------------------------------------------------------------------------
void PhantomBridge::on_full_position_set(const std::string& /*fen*/) {
    // v0.3.0 full-position sync would mean sending the gameStart 10×10 board
    // matrix; the board's orientation transform isn't byte-verified yet, so we
    // skip it for now (per-move drive below keeps a game-from-start in sync).
    // The user positions the physical board manually before a reset/undo.
    std::fprintf(stderr,
        "[phantom/native] on_full_position_set ignored "
        "(v1 drives moves one at a time)\n");
}

void PhantomBridge::on_move_played(const std::string& fen,
                                   int src_col, int src_row,
                                   int dst_col, int dst_row,
                                   bool capture) {
    // app_state hands us the app's INTERNAL columns (col 7 = a-file). The
    // Phantom wire format is algebraic, so convert col → file (file = 7 - col).
    int src_file = 7 - src_col;
    int dst_file = 7 - dst_col;
    // Advisory piece letter: the moved piece sits on the destination square in
    // the post-move FEN. Firmware tracks pieces itself, so 'E' (unknown) is a
    // safe fallback.
    char piece = phantom::fen_piece_at(fen, dst_file, dst_row);
    impl_->send_move(src_file, src_row, dst_file, dst_row,
                     capture, piece ? piece : 'E');
}

#endif  // !__EMSCRIPTEN__
