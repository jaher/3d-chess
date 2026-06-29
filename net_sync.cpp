// Native move-sync client — see net_sync.h.
//
// A small TCP client to the sync-server.js relay: a background reader thread
// receives newline-delimited JSON and marshals each remote move/reset onto the
// GLib main thread (g_idle_add) so the registered callbacks can touch AppState
// safely. Outgoing messages are written directly from the main thread.
#include "net_sync.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <glib.h>

namespace {
NetSyncMoveCb g_on_move = nullptr;
NetSyncResetCb g_on_reset = nullptr;
NetSyncPeerCb g_on_peer = nullptr;
NetSyncRoleCb g_on_role = nullptr;

std::atomic<int> g_fd{-1};
std::atomic<bool> g_run{false};
std::thread g_reader;

// --- tiny JSON field extraction (our relay messages are flat + controlled) --
// Returns the string value of "key":"value", or "" if absent.
std::string json_str(const std::string& s, const char* key) {
    std::string pat = std::string("\"") + key + "\":\"";
    size_t i = s.find(pat);
    if (i == std::string::npos) return "";
    i += pat.size();
    size_t j = s.find('"', i);
    if (j == std::string::npos) return "";
    return s.substr(i, j - i);
}
// Returns true/false for "key":true / "key":false; def if absent.
bool json_bool(const std::string& s, const char* key, bool def) {
    std::string pat = std::string("\"") + key + "\":";
    size_t i = s.find(pat);
    if (i == std::string::npos) return def;
    i += pat.size();
    while (i < s.size() && (s[i] == ' ')) i++;
    return s.compare(i, 4, "true") == 0;
}

// Marshal a received message to the main thread. We pass a heap copy of the
// raw line and re-parse there so only one thread touches the callbacks.
gboolean dispatch_line(gpointer data) {
    std::string* line = static_cast<std::string*>(data);
    const std::string& s = *line;
    std::string type = json_str(s, "type");
    if (type == "move") {
        std::string uci = json_str(s, "uci");
        if (!uci.empty() && g_on_move) g_on_move(uci.c_str());
    } else if (type == "reset") {
        if (g_on_reset) g_on_reset(json_bool(s, "fromWhite", true));
    } else if (type == "role") {
        if (g_on_role) g_on_role(json_bool(s, "initiator", false));
    } else if (type == "peer") {
        if (g_on_peer) g_on_peer(json_bool(s, "joined", false));
    }
    delete line;
    return G_SOURCE_REMOVE;
}

void reader_loop() {
    std::string acc;
    char buf[1024];
    while (g_run.load()) {
        int fd = g_fd.load();
        if (fd < 0) break;
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;                 // peer closed or error
        acc.append(buf, static_cast<size_t>(n));
        size_t nl;
        while ((nl = acc.find('\n')) != std::string::npos) {
            std::string line = acc.substr(0, nl);
            acc.erase(0, nl + 1);
            if (!line.empty())
                g_idle_add(dispatch_line, new std::string(line));   // -> main thread
        }
    }
    if (g_on_peer) {   // connection dropped — notify on the main thread
        g_idle_add([](gpointer) -> gboolean { if (g_on_peer) g_on_peer(false); return G_SOURCE_REMOVE; }, nullptr);
    }
}

bool send_raw(const std::string& s) {
    int fd = g_fd.load();
    if (fd < 0) return false;
    std::string line = s + "\n";
    size_t off = 0;
    while (off < line.size()) {
        ssize_t n = ::send(fd, line.data() + off, line.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}
}  // namespace

void net_sync_init(NetSyncMoveCb on_move, NetSyncResetCb on_reset,
                   NetSyncPeerCb on_peer, NetSyncRoleCb on_role) {
    g_on_move = on_move; g_on_reset = on_reset; g_on_peer = on_peer; g_on_role = on_role;
}

bool net_sync_connect(const char* host, int port, const char* room, const char* name) {
    net_sync_disconnect();
    struct addrinfo hints {}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    char portstr[16]; std::snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        std::fprintf(stderr, "[sync] cannot resolve %s:%d\n", host, port);
        return false;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return false; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        std::fprintf(stderr, "[sync] connect to %s:%d failed\n", host, port);
        close(fd); freeaddrinfo(res); return false;
    }
    freeaddrinfo(res);
    g_fd.store(fd);
    g_run.store(true);
    // join the room
    std::string join = std::string("{\"type\":\"join\",\"room\":\"") + room +
                       "\",\"name\":\"" + (name ? name : "desktop") + "\"}";
    send_raw(join);
    g_reader = std::thread(reader_loop);
    std::fprintf(stderr, "[sync] connected to %s:%d room=%s\n", host, port, room);
    return true;
}

void net_sync_send_move(const char* uci) {
    if (!uci || g_fd.load() < 0) return;
    send_raw(std::string("{\"type\":\"move\",\"uci\":\"") + uci + "\"}");
}
void net_sync_send_reset(bool am_white) {
    send_raw(std::string("{\"type\":\"reset\",\"fromWhite\":") + (am_white ? "true" : "false") + "}");
}

void net_sync_disconnect() {
    g_run.store(false);
    int fd = g_fd.exchange(-1);
    if (fd >= 0) { shutdown(fd, SHUT_RDWR); close(fd); }
    if (g_reader.joinable()) g_reader.join();
}

bool net_sync_active() { return g_fd.load() >= 0; }
