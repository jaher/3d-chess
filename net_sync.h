// Native (desktop) move-sync client for glasses <-> desktop play.
//
// The glasses Web App cannot use Bluetooth (no Web Bluetooth in its sandbox),
// so glasses<->desktop sync goes over the network: the glasses talk WebSocket
// to the sync-server.js relay, and this client joins the same relay "room" over
// raw TCP (newline-delimited JSON). Outgoing local moves are sent via
// net_sync_send_move (wired to AppPlatform::trigger_send_move); incoming remote
// moves/resets are delivered on the GLib main thread through the callbacks
// registered with net_sync_init, so they can safely touch AppState.
#pragma once

typedef void (*NetSyncMoveCb)(const char* uci);
typedef void (*NetSyncResetCb)(bool from_white);   // initiator's colour; adopt the opposite
typedef void (*NetSyncPeerCb)(bool joined);
typedef void (*NetSyncRoleCb)(bool initiator);     // relay-assigned: first in room = initiator

// Register main-thread callbacks (called once, before connecting).
void net_sync_init(NetSyncMoveCb on_move, NetSyncResetCb on_reset,
                   NetSyncPeerCb on_peer, NetSyncRoleCb on_role);

// Connect to the relay and join `room`. Returns false if the socket can't be
// opened. `name` is a label shown in the relay log.
bool net_sync_connect(const char* host, int port, const char* room, const char* name);

// Outgoing messages (safe to call from the main thread).
void net_sync_send_move(const char* uci);   // matches trigger_send_move signature
void net_sync_send_reset(bool am_white);

void net_sync_disconnect();
bool net_sync_active();
