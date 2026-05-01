// Standalone SimpleBLE scan diagnostic. If `./chess`'s in-app
// picker can't see the board but `python3 tools/chessnut_bridge.py`
// (bleak) can, this binary tells us whether the problem is in
// SimpleBLE or in the chess app's bridge glue.
//
// Build: make simpleble_scan
// Run:   ./tools/simpleble_scan

#include <simpleble/SimpleBLE.h>
#include <simpleble/Adapter.h>
#include <simpleble/Peripheral.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <set>
#include <string>
#include <thread>

int main() {
    if (!SimpleBLE::Adapter::bluetooth_enabled()) {
        std::fprintf(stderr,
            "FATAL  SimpleBLE::Adapter::bluetooth_enabled() == false\n"
            "       Likely cause: BlueZ not running, no permission, "
            "or hci0 powered off.\n"
            "       Try: bluetoothctl power on\n");
        return 1;
    }
    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) {
        std::fprintf(stderr,
            "FATAL  SimpleBLE::Adapter::get_adapters() returned empty.\n"
            "       BlueZ visible to bluetoothctl but not SimpleBLE = "
            "D-Bus permission / config issue.\n");
        return 1;
    }
    SimpleBLE::Adapter adapter = adapters.front();
    std::fprintf(stderr, "OK     adapter %s (%s)\n",
        adapter.identifier().c_str(), adapter.address().c_str());

    std::set<std::string> seen;
    adapter.set_callback_on_scan_found(
        [&seen](SimpleBLE::Peripheral p) {
            std::string addr = p.address();
            if (addr.empty() || !seen.insert(addr).second) return;
            std::string name = p.identifier();
            // Filter to peripherals whose name contains "chessnut"
            // (case-insensitive), matching the in-app picker.
            std::string lname;
            lname.reserve(name.size());
            for (char c : name) {
                lname.push_back(static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c))));
            }
            if (lname.find("chessnut") == std::string::npos) return;
            std::printf("DEVICE %s  %s\n",
                addr.c_str(),
                name.empty() ? "(no name)" : name.c_str());
            std::fflush(stdout);
        });

    std::fprintf(stderr, "OK     scanning 10 s — advertise the board now\n");
    adapter.scan_for(10000);
    std::fprintf(stderr, "OK     scan complete (%zu unique devices)\n",
                 seen.size());
    return 0;
}
