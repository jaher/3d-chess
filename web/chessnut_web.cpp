// Web Bluetooth driver for the Chessnut Move physical board. Mirrors
// the desktop SimpleBLE implementation in chessnut_bridge_native.cpp,
// but uses the browser's navigator.bluetooth API instead. Encoder
// shared via chessnut_encode.h so the wire format can't drift.
//
// Requires a browser with Web Bluetooth support — Chrome / Edge /
// Opera (desktop + Android). Safari (macOS / iOS) and Firefox don't
// implement the API; the toggle hides itself there via
// app_chessnut_supported().
//
// Web Bluetooth has two important constraints:
//   1. requestDevice() must be called from a user-gesture handler.
//      Our toggle is fired from release_options() (a click handler),
//      which satisfies this.
//   2. The page must be served over HTTPS (or localhost) — Web
//      Bluetooth refuses to run on plain HTTP.

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <emscripten/em_macros.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "../app_state.h"
#include "../chessnut_encode.h"

// AppState singleton trampoline — defined in web/voice_web.cpp,
// bound by main_web.cpp's chess_start(). Reuse here so we don't
// duplicate the binding plumbing.
extern AppState& web_app();

// ---------------------------------------------------------------------------
// JS-side glue. Holds the connected device on window.__chessnutBoard
// (or null if no connection). Status is reported back through the
// EMSCRIPTEN_KEEPALIVE callback below.
// ---------------------------------------------------------------------------
EM_JS(int, chessnut_web_supported_js, (), {
    return (typeof navigator !== "undefined" && !!navigator.bluetooth)
        ? 1 : 0;
});

EM_JS(void, chessnut_web_start_js, (), {
    if (window.__chessnutBoard) {
        Module.ccall("on_chessnut_status_from_js", null, ["string"],
                     ["CONNECTED " + (window.__chessnutBoard.name || "Chessnut Move")]);
        return;
    }
    var SUFFIX = "-2877-41c3-b46e-cf057c562023";
    var WRITE  = "1b7e8272" + SUFFIX;
    var NOTIFY = ["1b7e8261" + SUFFIX,
                  "1b7e8262" + SUFFIX,
                  "1b7e8271" + SUFFIX,
                  "1b7e8273" + SUFFIX];
    // The parent service UUID isn't fixed across firmware revisions
    // — desktop discovers it dynamically via SimpleBLE. Web
    // Bluetooth needs every candidate listed in optionalServices
    // upfront, so we sweep the 1b7e82XX hex range that contains
    // every known Chessnut characteristic (8261, 8262, 8271, 8272,
    // 8273). The parent service is almost certainly in this range
    // too. 256 entries — well within Web Bluetooth's per-device
    // limits.
    var CANDIDATE_SVCS = [];
    for (var v = 0; v < 256; v++) {
        var h = v.toString(16);
        if (h.length < 2) h = "0" + h;
        CANDIDATE_SVCS.push("1b7e82" + h + SUFFIX);
    }
    // Three-frame post-subscribe handshake — see
    // chessnut_bridge_native.cpp do_connect for the rationale.
    // 0x21 0x01 0x00 is the streaming-enable command without which
    // the firmware never pushes board-state notifications.
    var INIT_HANDSHAKE_0 = new Uint8Array([0x21, 0x01, 0x00]);
    var INIT_HANDSHAKE_1 = new Uint8Array([0x0B, 0x04, 0x03, 0xE8, 0x00, 0xC8]);
    var INIT_HANDSHAKE_2 = new Uint8Array([0x27, 0x01, 0x00]);

    function emit(s) {
        Module.ccall("on_chessnut_status_from_js", null, ["string"], [s]);
    }

    navigator.bluetooth.requestDevice({
        filters: [{ namePrefix: "Chessnut" }],
        optionalServices: CANDIDATE_SVCS
    }).then(function(device) {
        device.addEventListener("gattserverdisconnected", function() {
            window.__chessnutBoard = null;
            emit("DISCONNECTED");
        });
        return device.gatt.connect().then(function(server) {
            // Enumerate every primary service the browser exposes
            // (limited to the optionalServices declared above), then
            // find the one carrying the WRITE characteristic. That's
            // the parent service for all our notify channels too.
            return server.getPrimaryServices().then(function(services) {
                console.log("[chessnut/web] services:", services.map(function(s){return s.uuid;}));
                return Promise.all(services.map(function(s) {
                    return s.getCharacteristic(WRITE)
                        .then(function(c) { return { service: s, write: c }; })
                        .catch(function() { return null; });
                })).then(function(results) {
                    var found = null;
                    for (var i = 0; i < results.length; ++i) {
                        if (results[i]) { found = results[i]; break; }
                    }
                    if (!found) {
                        emit("ERROR write characteristic " + WRITE +
                             " not found on any primary service " +
                             "(saw " + services.length + " services)");
                        return null;
                    }
                    var service = found.service;
                    var write   = found.write;
                    console.log("[chessnut/web] using service:", service.uuid);

                    window.__chessnutBoard = {
                        device:  device,
                        service: service,
                        write:   write,
                        name:    device.name || "Chessnut Move"
                    };
                    // Sweep every primary service we discovered
                    // for notify-capable characteristics and
                    // subscribe to all of them. Field firmware
                    // exposes board-state pushes on different
                    // UUIDs across revisions; the size-based
                    // filter in app_chessnut_apply_sensor_frame
                    // ignores everything that isn't a 32-byte
                    // board frame, so over-subscribing is safe.
                    var subs = services.map(function(svc) {
                        return svc.getCharacteristics()
                            .then(function(chars) {
                                return Promise.all(chars.map(function(c) {
                                    if (!c.properties || !c.properties.notify) {
                                        return null;
                                    }
                                    return c.startNotifications()
                                        .then(function() {
                                            c.addEventListener(
                                                "characteristicvaluechanged",
                                                function(ev) {
                                                    var v = ev.target.value;
                                                    var hex = "";
                                                    for (var i = 0; i < v.byteLength; ++i) {
                                                        var b = v.getUint8(i).toString(16);
                                                        hex += (b.length < 2 ? "0" : "") + b;
                                                    }
                                                    emit("NOTIFY " + c.uuid + " " + hex);
                                                });
                                        })
                                        .catch(function() {});
                                }));
                            })
                            .catch(function() { return null; });
                    });
                    return Promise.all(subs).then(function() {
                        return write.writeValueWithoutResponse(INIT_HANDSHAKE_0)
                            .then(function() { return write.writeValueWithoutResponse(INIT_HANDSHAKE_1); })
                            .then(function() { return write.writeValueWithoutResponse(INIT_HANDSHAKE_2); })
                            .then(function() { emit("CONNECTED " + (device.name || "Chessnut Move")); });
                    });
                });
            });
        });
    }).catch(function(err) {
        var msg = (err && err.message) ? err.message : String(err);
        emit("ERROR " + msg);
    });
});

EM_JS(void, chessnut_web_stop_js, (), {
    var b = window.__chessnutBoard;
    window.__chessnutBoard = null;
    if (b && b.device && b.device.gatt && b.device.gatt.connected) {
        try { b.device.gatt.disconnect(); } catch (e) {}
    }
});

// Send a raw frame. We pass the WASM heap pointer + length and let
// JS slice into it (the slice is necessary because we're handing
// the buffer to an async API; without slicing, growth of the WASM
// heap could detach the typed array view).
EM_JS(int, chessnut_web_write_frame_js, (const uint8_t* ptr, int len), {
    var b = window.__chessnutBoard;
    if (!b || !b.write) return 0;
    var arr = new Uint8Array(HEAPU8.buffer, ptr, len).slice();
    b.write.writeValueWithoutResponse(arr).catch(function(err) {
        var msg = (err && err.message) ? err.message : String(err);
        Module.ccall("on_chessnut_status_from_js", null, ["string"],
                     ["ERROR write: " + msg]);
    });
    return 1;
});

// ---------------------------------------------------------------------------
// JS → C bridge. Every status line goes through here and is then
// handed off to the shared app_chessnut_apply_status path.
// ---------------------------------------------------------------------------
extern "C" EMSCRIPTEN_KEEPALIVE
void on_chessnut_status_from_js(const char* status) {
    app_chessnut_apply_status(web_app(),
                              status ? std::string(status) : std::string());
}

// ---------------------------------------------------------------------------
// app_chessnut_* implementations for the web build.
// ---------------------------------------------------------------------------
bool app_chessnut_supported() {
    return chessnut_web_supported_js() != 0;
}

void app_chessnut_toggle_request(AppState& a) {
    bool target = !a.chessnut_enabled;
    // Status callback unused on web — results flow through the
    // EMSCRIPTEN_KEEPALIVE function above. Pass an empty lambda so
    // the shared signature is satisfied.
    app_chessnut_set_enabled(a, target, [](const std::string&) {});
}

void app_chessnut_set_enabled(
    AppState& a, bool on,
    std::function<void(const std::string&)> /*on_status*/) {
    if (on == a.chessnut_enabled) return;

    if (!on) {
        chessnut_web_stop_js();
        a.chessnut_enabled        = false;
        a.chessnut_bridge_running = false;
        a.chessnut_connected      = false;
        if (a.platform && a.platform->set_status)
            a.platform->set_status("Chessnut Move: off");
        return;
    }

    if (!app_chessnut_supported()) {
        if (a.platform && a.platform->set_status)
            a.platform->set_status(
                "Chessnut Move unavailable — browser has no Web Bluetooth");
        return;
    }
    a.chessnut_enabled        = true;
    a.chessnut_bridge_running = true;
    a.chessnut_connected      = false;
    if (a.platform && a.platform->set_status)
        a.platform->set_status("Chessnut Move: choose device…");
    chessnut_web_start_js();
}

void app_chessnut_sync_board(AppState& a, bool force) {
    if (!a.chessnut_enabled || !a.chessnut_connected) return;
    std::string fen = app_current_fen(a);
    std::vector<uint8_t> frame;
    try {
        frame = chessnut::make_set_move_board(fen, force);
    } catch (const std::exception& e) {
        if (a.platform && a.platform->set_status)
            a.platform->set_status(
                (std::string("Chessnut Move: encode error — ") + e.what()).c_str());
        return;
    }
    chessnut_web_write_frame_js(frame.data(), static_cast<int>(frame.size()));
}

void app_chessnut_shutdown(AppState& a) {
    a.chessnut_enabled        = false;
    a.chessnut_bridge_running = false;
    a.chessnut_connected      = false;
    chessnut_web_stop_js();
}

// LED hints — defined as a no-op on web for now. Wiring it up is
// straightforward (same EM_JS shim pattern as send_fen, just a
// different opcode), kept out of scope while we get the move-by-
// move sync verified on physical hardware.
void app_chessnut_highlight_last_move(AppState& /*a*/) {}

// Web doesn't need the in-app picker — navigator.bluetooth.requestDevice
// already pops a browser-native chooser dialog. These exist so the
// shared header signature is satisfied.
void app_chessnut_open_picker(AppState& /*a*/)  {}
void app_chessnut_close_picker(AppState& /*a*/) {}
// The browser caches its own pairing permission. There's no
// equivalent of our ~/.cache MAC file, so this is a no-op on web.
void app_chessnut_forget_cached_device(AppState& /*a*/) {}
void app_chessnut_pick_device(AppState& a, const std::string& /*addr*/) {
    // No-op: the browser already picked a device when set_enabled fired.
    // If callers ever route here on web, just turn the toggle on.
    if (!a.chessnut_enabled) {
        app_chessnut_set_enabled(a, true,
                                 [](const std::string&) {});
    }
}

#endif  // __EMSCRIPTEN__
