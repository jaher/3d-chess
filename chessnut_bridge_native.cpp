// Chessnut Move BLE bridge — native (in-process) implementation
// using SimpleBLE. Default impl since SimpleBLE removes the
// python3 + bleak runtime dependency. The Python subprocess
// implementation in chessnut_bridge_python.cpp is kept as an
// escape hatch (set CHESS_CHESSNUT_USE_PYTHON=1).

#ifndef __EMSCRIPTEN__

#include "chessnut_bridge_impl.h"
#include "chessnut_encode.h"  // shared with web/chessnut_web.cpp

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <simpleble/Service.h>
#include <simpleble/Characteristic.h>
#include <mutex>
#include <set>
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
// GATT — see PROTOCOL.md in the repo root. Same characteristic
// UUID family as the Chessnut Air community-RE'd protocol; Move
// adds the 8261/8271 pair.
//
// Only the characteristic UUIDs are stable. The parent service
// UUID isn't — it was originally guessed as 1b7e8260-… and the
// firmware (at least the unit tested in the field) uses something
// else. On every connect we enumerate the peripheral's actual
// services + characteristics and route writes/notifies via the
// parent service we discover, so we never hardcode it again.
// ---------------------------------------------------------------------------
constexpr const char* SUFFIX = "-2877-41c3-b46e-cf057c562023";
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

template <size_t N>
SimpleBLE::ByteArray to_byte_array(const std::array<uint8_t, N>& bytes) {
    SimpleBLE::ByteArray out;
    out.reserve(N);
    for (uint8_t b : bytes) out.push_back(static_cast<uint8_t>(b));
    return out;
}

// Last-connected MAC is cached so subsequent toggles go straight
// to the right peripheral instead of doing a fresh 8 s scan. Path
// matches the Python helper's so they share the cache: flipping
// the toggle in the desktop app and then running the standalone
// helper for protocol experiments both Just Work.
std::string address_cache_path() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return std::string();
    return std::string(home) + "/.cache/chessnut_bridge_address";
}

std::string load_cached_address() {
    if (const char* env = std::getenv("CHESS_CHESSNUT_ADDRESS"))
        if (*env) return env;
    std::string path = address_cache_path();
    if (path.empty()) return "";
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return "";
    char buf[64];
    std::string out;
    if (std::fgets(buf, sizeof(buf), f)) out = buf;
    std::fclose(f);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

void save_cached_address(const std::string& addr) {
    std::string path = address_cache_path();
    if (path.empty() || addr.empty()) return;
    // Best-effort mkdir of the cache dir.
    std::string dir = path.substr(0, path.find_last_of('/'));
    if (!dir.empty()) {
        std::string cmd = "mkdir -p " + dir + " 2>/dev/null";
        int unused = std::system(cmd.c_str()); (void)unused;
    }
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    std::fputs((addr + "\n").c_str(), f);
    std::fclose(f);
}

// ---------------------------------------------------------------------------
// Bridge — runs all SimpleBLE work on a dedicated worker thread.
// SimpleBLE's API is mostly synchronous (scan_for blocks, connect
// blocks, write_command blocks); driving it from a worker keeps the
// GTK main thread unblocked.
// ---------------------------------------------------------------------------
struct Command {
    enum Kind { CONNECT, SCAN, CONNECT_TO, FEN, FEN_FORCE, LED,
                PROBE_PIECE_STATE, QUIT };
    Kind        kind;
    std::string arg;     // FEN string, LED hex, or MAC for CONNECT_TO
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

    void request_connect() override     { enqueue({Command::CONNECT,    ""}); }
    void start_scan() override          { enqueue({Command::SCAN,       ""}); }
    void connect_to_address(const std::string& addr) override {
        enqueue({Command::CONNECT_TO, addr});
    }
    void send_fen(const std::string& fen, bool force) override {
        enqueue({force ? Command::FEN_FORCE : Command::FEN, fen});
    }
    void send_led_hex(const std::string& hex) override {
        enqueue({Command::LED, hex});
    }
    void probe_piece_state() override {
        enqueue({Command::PROBE_PIECE_STATE, ""});
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
        // Mirror to stderr so we can see what the bridge is doing
        // even when the GUI status bar is hidden behind menus.
        std::fprintf(stderr, "[chessnut/native] %s\n", s.c_str());
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
                case Command::CONNECT:    do_connect(""); break;
                case Command::CONNECT_TO: do_connect(c.arg); break;
                case Command::SCAN:       do_scan(); break;
                case Command::FEN:
                    do_write(to_byte_array(chessnut::make_set_move_board(c.arg, false)), "FEN");
                    break;
                case Command::FEN_FORCE:
                    do_write(to_byte_array(chessnut::make_set_move_board(c.arg, true)), "FEN_FORCE");
                    break;
                case Command::LED:
                    do_write(to_byte_array(chessnut::make_led_frame(c.arg)), "LED");
                    break;
                case Command::PROBE_PIECE_STATE:
                    // getMovePieceState — the firmware replies on a
                    // notify channel with what it thinks the current
                    // piece layout is. Diagnostic only.
                    do_write(to_byte_array(chessnut::CMD_GET_PIECE_STATE),
                             "PROBE_PIECE_STATE");
                    break;
                }
            } catch (const std::exception& e) {
                emit(std::string("ERROR ") + e.what());
            } catch (...) {
                emit("ERROR unknown failure");
            }
        }
    }

    void do_scan() {
        if (!SimpleBLE::Adapter::bluetooth_enabled()) {
            emit("ERROR Bluetooth disabled or unavailable");
            emit("SCAN_COMPLETE");
            return;
        }
        auto adapters = SimpleBLE::Adapter::get_adapters();
        if (adapters.empty()) {
            emit("ERROR no BLE adapter");
            emit("SCAN_COMPLETE");
            return;
        }
        SimpleBLE::Adapter adapter = adapters.front();

        // Track addresses we've already reported so we don't spam
        // the picker if the same device fires multiple advertising
        // events during the scan window. Filter to peripherals
        // whose name contains "chessnut" (case-insensitive) — the
        // picker has finite vertical space and a typical office
        // BLE scan turns up 20+ irrelevant devices. For an
        // unfiltered view, use tools/simpleble_scan.
        std::set<std::string> seen;
        adapter.set_callback_on_scan_found(
            [&](SimpleBLE::Peripheral p) {
                std::string addr, name;
                try {
                    addr = p.address();
                    name = p.identifier();
                } catch (...) { return; }
                if (addr.empty()) return;
                std::string lname;
                lname.reserve(name.size());
                for (char c : name) {
                    lname.push_back(static_cast<char>(
                        std::tolower(static_cast<unsigned char>(c))));
                }
                if (lname.find("chessnut") == std::string::npos) return;
                if (!seen.insert(addr).second) return;
                if (name.empty()) name = "(no name)";
                emit("DEVICE " + addr + " " + name);
            });
        adapter.scan_for(8000);
        emit("SCAN_COMPLETE");
    }

    // Disconnect (best-effort) and clear all peripheral-side state
    // so the next connect attempt starts from a clean slate.
    // Anything that left us "half-connected" (services() threw,
    // WRITE_UUID missing, handshake failed) routes through here.
    void teardown_peripheral() {
        if (peripheral_initialised_) {
            try {
                if (peripheral_.is_connected()) peripheral_.disconnect();
            } catch (...) { /* best-effort */ }
        }
        peripheral_initialised_ = false;
        char_to_service_.clear();
    }

    // Connect to the board. If `explicit_addr` is non-empty, we
    // skip name matching and target that MAC directly (used by the
    // picker after the user picks a row, and as a fast path when
    // the cache matches). Empty string means "use cached MAC, then
    // fall back to name-prefix scan" — the auto-toggle path.
    void do_connect(const std::string& explicit_addr) {
        // Healthy reuse: peripheral is connected AND we know how to
        // route writes (i.e. discovery completed). The empty-map
        // check is load-bearing — without it a previous discovery
        // failure leaves us re-emitting CONNECTED on every retry
        // while every subsequent write fails with "write
        // characteristic not discovered".
        if (peripheral_initialised_ && peripheral_.is_connected() &&
            !char_to_service_.empty()) {
            emit("CONNECTED " + connected_name_);
            return;
        }
        // Stale / half-connected from a prior failure. Tear down so
        // the scan + connect path below runs fresh.
        if (peripheral_initialised_) teardown_peripheral();
        if (!SimpleBLE::Adapter::bluetooth_enabled()) {
            emit("ERROR Bluetooth disabled or unavailable");
            return;
        }
        auto adapters = SimpleBLE::Adapter::get_adapters();
        if (adapters.empty()) { emit("ERROR no BLE adapter"); return; }
        SimpleBLE::Adapter adapter = adapters.front();

        std::string want_addr = explicit_addr.empty()
            ? load_cached_address() : explicit_addr;
        SimpleBLE::Peripheral target;
        bool found = false;
        auto take = [&](SimpleBLE::Peripheral p, bool by_address) {
            if (found) return;
            try {
                if (by_address) {
                    if (p.address() != want_addr) return;
                } else {
                    std::string name = p.identifier();
                    if (!(name.rfind("Chessnut", 0) == 0)) return;
                }
            } catch (...) { return; }
            target = p;
            found  = true;
        };

        if (!want_addr.empty()) {
            adapter.set_callback_on_scan_found(
                [&](SimpleBLE::Peripheral p) { take(p, /*by_address=*/true); });
            adapter.scan_for(2500);
        }
        // Only fall back to name-prefix when the toggle had no
        // explicit address to chase. With an explicit address from
        // the picker we want a clean "no, that exact device wasn't
        // there" failure rather than a silent wrong-device
        // connect.
        if (!found && explicit_addr.empty()) {
            adapter.set_callback_on_scan_found(
                [&](SimpleBLE::Peripheral p) { take(p, /*by_address=*/false); });
            adapter.scan_for(8000);
        }
        if (!found) {
            emit("ERROR no Chessnut device found");
            return;
        }
        connected_name_ = target.identifier();
        target.set_callback_on_disconnected([this] {
            emit("DISCONNECTED");
        });
        target.connect();
        peripheral_ = target;
        peripheral_initialised_ = true;

        // Discover the actual parent service for each
        // characteristic. Hardcoding "1b7e8260-…" was wrong on the
        // tested firmware; the real service UUID can vary, so we
        // map by characteristic and route writes/notifies via the
        // matching parent.
        //
        // BlueZ sometimes reports `connect()` as complete before its
        // GATT object tree is fully resolved, surfacing as either
        // "D-Bus call timed out" or "Path /…/serviceXXXX does not
        // contain interface org.bluez.GattService1". Retry a few
        // times with a short sleep — the second attempt almost
        // always succeeds once BlueZ catches up.
        char_to_service_.clear();
        std::vector<std::pair<std::string, std::string>> notify_chars;
        std::vector<SimpleBLE::Service> services;
        std::string last_error;
        constexpr int kDiscoveryAttempts = 4;
        for (int attempt = 0; attempt < kDiscoveryAttempts; attempt++) {
            try {
                services = peripheral_.services();
                last_error.clear();
                if (!services.empty()) break;
                last_error = "no services returned";
            } catch (const std::exception& e) {
                last_error = e.what();
            }
            std::fprintf(stderr,
                "[chessnut/native] services() attempt %d/%d failed: %s\n",
                attempt + 1, kDiscoveryAttempts, last_error.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (!last_error.empty()) {
            emit(std::string("ERROR service discovery: ") + last_error);
            teardown_peripheral();
            return;
        }
        std::fprintf(stderr,
            "[chessnut/native] services discovered: %zu\n",
            services.size());
        for (auto& s : services) {
            for (auto& c : s.characteristics()) {
                char_to_service_[c.uuid()] = s.uuid();
                bool n = false;
                try { n = c.can_notify() || c.can_indicate(); }
                catch (...) { n = false; }
                std::fprintf(stderr,
                    "[chessnut/native]     char %s%s\n",
                    c.uuid().c_str(), n ? " [NOTIFY]" : "");
                if (n) notify_chars.emplace_back(s.uuid(), c.uuid());
            }
            std::fprintf(stderr,
                "[chessnut/native]   service %s (%zu chars)\n",
                s.uuid().c_str(), s.characteristics().size());
        }

        auto svc_for = [this](const std::string& char_uuid)
                              -> const std::string* {
            auto it = char_to_service_.find(char_uuid);
            return it == char_to_service_.end() ? nullptr : &it->second;
        };

        if (!svc_for(WRITE_UUID)) {
            emit(std::string("ERROR write characteristic ") + WRITE_UUID +
                 " not exposed by this peripheral");
            teardown_peripheral();
            return;
        }

        // Subscribe to every notify-capable characteristic the
        // peripheral exposes — the Chessnut Move firmware revision
        // varies which characteristic carries board-state pushes
        // (we've seen 8262 on Air-class firmware and a different
        // UUID on Move). The size-based filter in
        // app_chessnut_apply_sensor_frame ignores anything that
        // isn't a 32-byte board frame, so over-subscribing is
        // harmless and our diagnostic trace logs every NOTIFY
        // payload by UUID anyway.
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

        // Log the negotiated MTU. Android explicitly requests 500;
        // SimpleBLE has no setter (BlueZ negotiates implicitly), so
        // we just observe and log. Default ATT_MTU is 23 (max 20-
        // byte payload per write); our 0x42 setMoveBoard frame is
        // 35 bytes and would need long-write fragmentation if MTU
        // hasn't grown past ~37. If the value here is small, the
        // motors-don't-move bug is almost certainly an MTU problem.
        try {
            uint16_t mtu = peripheral_.mtu();
            std::fprintf(stderr,
                "[chessnut/native] negotiated MTU = %u\n",
                static_cast<unsigned>(mtu));
        } catch (...) { /* MTU getter is best-effort */ }

        // Two-frame post-subscribe handshake. From the Android app's
        // onConnect sequence (ChessnutBLEDevice.java:339, 342). The
        // Android app then conditionally sends OPCODE_LEGACY_INIT —
        // but ONLY when the device name does NOT contain "Chessnut"
        // (i.e. for legacy unbranded firmware). Our target advertises
        // as "Chessnut Move", so the Java code skips the third write
        // and so do we.
        //   1. CMD_STREAM_ENABLE — without this, the firmware doesn't
        //      push sensor frames when pieces move.
        //   2. CMD_AUX_INIT — auxiliary init (constants from the
        //      firmware; semantics unknown but required).
        //
        // We add ~200 ms gaps between writes to mirror the Android
        // app's `bleWriteCondition.await(500ms)` pacing — back-to-
        // back writes appear to land but the firmware may need
        // settling time before the next opcode is interpreted.
        const std::string& write_svc = *svc_for(WRITE_UUID);
        try {
            peripheral_.write_request(write_svc, WRITE_UUID,
                to_byte_array(chessnut::CMD_STREAM_ENABLE));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            peripheral_.write_request(write_svc, WRITE_UUID,
                to_byte_array(chessnut::CMD_AUX_INIT));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        } catch (const std::exception& e) {
            emit(std::string("ERROR handshake failed: ") + e.what());
            teardown_peripheral();
            return;
        }

        // Diagnostic probe — ask the firmware what pieces it sees.
        // Reply lands as a NOTIFY which we log raw. Lets us tell
        // "firmware ignores motor commands entirely" apart from
        // "firmware thinks board is already at target".
        try {
            peripheral_.write_request(write_svc, WRITE_UUID,
                to_byte_array(chessnut::CMD_GET_PIECE_STATE));
        } catch (...) { /* probe is best-effort */ }

        // Cache the MAC so the next CONNECT skips the slow scan.
        try { save_cached_address(target.address()); } catch (...) {}
        emit("CONNECTED " + connected_name_);
    }

    void do_write(const SimpleBLE::ByteArray& frame, const char* tag) {
        if (!peripheral_initialised_ || !peripheral_.is_connected()) {
            emit("ERROR not connected");
            return;
        }
        auto it = char_to_service_.find(WRITE_UUID);
        if (it == char_to_service_.end()) {
            emit("ERROR write characteristic not discovered");
            return;
        }
        // write_request (with response) — see do_connect for the
        // rationale vs write_command.
        std::fprintf(stderr, "[chessnut/native] write %s len=%zu\n",
                     tag, frame.size());
        try {
            peripheral_.write_request(it->second, WRITE_UUID, frame);
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
    // Discovered on connect: characteristic UUID → parent service
    // UUID. Looked up before every write / notify subscription so
    // we don't need to hardcode the parent service.
    std::map<std::string, std::string> char_to_service_;
};

}  // namespace

std::unique_ptr<IChessnutBridgeImpl> make_chessnut_native_impl() {
    return std::make_unique<NativeImpl>();
}

#endif  // !__EMSCRIPTEN__
