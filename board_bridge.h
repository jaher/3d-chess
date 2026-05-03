#pragma once

// Abstract base class for physical-chessboard drivers. Two
// concrete implementations exist today:
//
//   * ChessnutBridge — Chessnut Move protocol (32-byte FEN frames,
//     RGB LED grid, sensor pushes).
//   * PhantomBridge  — Phantom Chessboard protocol (ASCII MOVE_CMD
//     strings, no per-move LED API, sensor format unverified).
//
// The picker tells the user-facing toggle which protocol the
// connected device speaks; app_state.cpp owns a single
// `IBoardBridge*` "active bridge" pointer and dispatches every
// per-move event through this interface so the rest of the app
// doesn't have to think about which protocol is in play.
//
// This header is intentionally lean — the things every protocol
// needs to expose are the per-move event hooks and the lifecycle
// triple (start / stop / connect). Protocol-specific helpers
// (chessnut LED grids, phantom raw move-string send, …) stay on
// the concrete classes and are reachable via a downcast or a
// dedicated reference if a caller really needs them.

#ifndef __EMSCRIPTEN__

#include <functional>
#include <string>

class IBoardBridge {
public:
    using StatusCallback = std::function<void(const std::string& status)>;

    virtual ~IBoardBridge() = default;

    // Lifecycle. start() spins up the worker thread; stop() joins
    // it. Idempotent: calling start() while already running just
    // refreshes the callback; calling stop() on a stopped bridge is
    // a no-op.
    virtual bool start(StatusCallback on_status) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;

    // Connect to a previously discovered peripheral by MAC. Async —
    // the result lands as a "CONNECTED <name>" or "ERROR …" status
    // message on the callback registered in start().
    virtual void connect_to_address(const std::string& address) = 0;

    // High-level per-move event hooks. The shared dispatch in
    // app_state.cpp fires these; the concrete bridge encodes them
    // however its protocol requires.
    //
    // on_full_position_set: a full position has just been loaded
    //   (game start, position reset, undo / redo). Chessnut Move
    //   pushes a setMoveBoard frame here; Phantom currently has no
    //   equivalent and treats this as a no-op (the user is expected
    //   to position the physical board manually before resetting
    //   the digital game).
    //
    // on_move_played: a single move has landed on the digital
    //   board. Chessnut Move pushes a fresh setMoveBoard frame
    //   (force=false, so the firmware replans from sensor state);
    //   Phantom sends a MOVE_CMD ASCII string. `capture` controls
    //   the firmware's pre-step lift-off-the-captured-piece
    //   handling on Phantom; on Chessnut it's diagnostic.
    virtual void on_full_position_set(const std::string& fen) = 0;
    virtual void on_move_played(const std::string& fen,
                                int src_col, int src_row,
                                int dst_col, int dst_row,
                                bool capture) = 0;

    // Optional last-move highlight (LED feedback). Default no-op
    // because not every protocol has a per-move LED API.
    virtual void on_highlight_move(int /*src_col*/, int /*src_row*/,
                                   int /*dst_col*/, int /*dst_row*/) {}

    // Human-readable label used in status messages and logs ("Chessnut
    // Move", "Phantom"). Not the device's advertised name — the brand
    // family.
    virtual const char* label() const = 0;
};

#endif  // !__EMSCRIPTEN__
