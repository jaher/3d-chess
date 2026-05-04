// Phantom Chessboard BLE bridge — SimpleBLE-backed. Siblings the
// Chessnut Move bridge in chessnut_bridge.cpp. Different protocol
// (single ASCII move-string write to a single characteristic) but
// the threading model is identical: a worker thread drains a
// command queue, all SimpleBLE calls happen there, status is
// surfaced through a callback registered by app_state.cpp.
//
// The Phantom protocol is partially verified — the motor-drive
// channel and write framing are confirmed from the firmware
// reverse-engineering documented in docs/PHANTOM.md. The notify-frame
// formats are NOT confirmed; this driver subscribes to all
// notify-capable characteristics and logs frames raw to stderr so
// they can be captured the first time someone with a real Phantom
// runs the app.

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

SimpleBLE::ByteArray to_byte_array(const std::string& s) {
    SimpleBLE::ByteArray out;
    out.reserve(s.size());
    for (unsigned char b : s) out.push_back(static_cast<uint8_t>(b));
    return out;
}

struct Command {
    enum Kind { CONNECT_TO, MOVE, QUIT };
    Kind kind;
    std::string addr;
    std::string move_payload;
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
        enqueue({Command::QUIT, "", ""});
        if (worker_.joinable()) worker_.join();
        on_status_ = nullptr;
    }

    void connect_to_address(const std::string& addr) {
        enqueue({Command::CONNECT_TO, addr, ""});
    }

    void send_move(int src_col, int src_row, int dst_col, int dst_row,
                   bool capture) {
        std::string payload = phantom::make_move_cmd(
            src_col, src_row, dst_col, dst_row, capture);
        enqueue({Command::MOVE, "", payload});
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
                case Command::MOVE:
                    do_send_move(c.move_payload);
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
        if (char_to_service_.find(phantom::MOVE_CMD_UUID)
            == char_to_service_.end()) {
            emit(std::string("ERROR move-cmd characteristic ")
                 + phantom::MOVE_CMD_UUID
                 + " not exposed by this peripheral");
            teardown_peripheral();
            return;
        }

        // Subscribe to every notify-capable characteristic. Without
        // a verified frame format we just log payloads raw — the
        // first user with a real Phantom can paste the trace into a
        // follow-up to lock in the parser.
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

        emit("CONNECTED " + connected_name_);
    }

    void do_send_move(const std::string& payload) {
        if (!peripheral_initialised_ || !peripheral_.is_connected()) {
            emit("ERROR not connected");
            return;
        }
        auto it = char_to_service_.find(phantom::MOVE_CMD_UUID);
        if (it == char_to_service_.end()) {
            emit("ERROR move-cmd characteristic not discovered");
            return;
        }
        std::fprintf(stderr,
            "[phantom/native] write MOVE_CMD len=%zu payload='%s'\n",
            payload.size(), payload.c_str());
        try {
            peripheral_.write_request(it->second, phantom::MOVE_CMD_UUID,
                                      to_byte_array(payload));
        } catch (const std::exception& e) {
            emit(std::string("ERROR write failed: ") + e.what());
            return;
        }
        emit("ACK MOVE");
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
void PhantomBridge::send_move(int src_col, int src_row,
                              int dst_col, int dst_row, bool capture) {
    impl_->send_move(src_col, src_row, dst_col, dst_row, capture);
}
bool PhantomBridge::running() const { return impl_->running(); }

// ---------------------------------------------------------------------------
// IBoardBridge polymorphic overrides.
// ---------------------------------------------------------------------------
void PhantomBridge::on_full_position_set(const std::string& /*fen*/) {
    // Phantom has no setMoveBoard primitive — moves only land one
    // at a time. A full reset would require us to choreograph each
    // piece's path individually, which we don't yet do. The user
    // resets the physical board manually.
    std::fprintf(stderr,
        "[phantom/native] on_full_position_set ignored "
        "(Phantom drives moves one at a time)\n");
}

void PhantomBridge::on_move_played(const std::string& /*fen*/,
                                   int src_col, int src_row,
                                   int dst_col, int dst_row,
                                   bool capture) {
    impl_->send_move(src_col, src_row, dst_col, dst_row, capture);
}

#endif  // !__EMSCRIPTEN__
