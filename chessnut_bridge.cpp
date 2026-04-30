// Chessnut Move BLE bridge — desktop-only subprocess wrapper.
// Mirrors the StockfishEngine pattern in ai_player.cpp.

#ifndef __EMSCRIPTEN__

#include "chessnut_bridge.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int make_pipe_cloexec(int pipefd[2]) {
#if defined(__linux__)
    return pipe2(pipefd, O_CLOEXEC);
#else
    if (pipe(pipefd) < 0) return -1;
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(pipefd[i], F_GETFD);
        if (flags < 0 || fcntl(pipefd[i], F_SETFD, flags | FD_CLOEXEC) < 0) {
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            return -1;
        }
    }
    return 0;
#endif
}

// Bridge script lives in tools/chessnut_bridge.py relative to the
// working directory. CHESS_CHESSNUT_BRIDGE overrides for development.
std::string bridge_script_path() {
    if (const char* env = std::getenv("CHESS_CHESSNUT_BRIDGE"))
        if (*env) return env;
    return "tools/chessnut_bridge.py";
}

std::string python_path() {
    if (const char* env = std::getenv("CHESS_PYTHON"))
        if (*env) return env;
    return "python3";
}

}  // namespace

ChessnutBridge::~ChessnutBridge() { stop(); }

bool ChessnutBridge::start(StatusCallback on_status) {
    if (pid_ > 0) return true;
    on_status_ = std::move(on_status);

    int in_pipe[2];
    int out_pipe[2];
    if (make_pipe_cloexec(in_pipe) < 0) return false;
    if (make_pipe_cloexec(out_pipe) < 0) {
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        return false;
    }

    if (pid == 0) {
        // child
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        // leave stderr attached so bleak/python tracebacks appear in
        // the parent terminal — useful for debugging the BLE stack.
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        ::close(out_pipe[0]); ::close(out_pipe[1]);

        std::string py = python_path();
        std::string script = bridge_script_path();
        // -u disables Python's stdout buffering so READY etc. arrive
        // immediately rather than on shutdown.
        execlp(py.c_str(), py.c_str(), "-u", script.c_str(),
               (char*)nullptr);
        std::fprintf(stderr,
            "ChessnutBridge: failed to exec %s %s: %s\n",
            py.c_str(), script.c_str(), std::strerror(errno));
        _exit(127);
    }

    // parent
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    in_fd_  = in_pipe[1];
    out_fd_ = out_pipe[0];
    pid_    = pid;

    signal(SIGPIPE, SIG_IGN);
    stop_reader_.store(false);
    reader_thread_ = std::thread([this] { reader_loop(); });
    return true;
}

void ChessnutBridge::stop() {
    if (pid_ > 0 && in_fd_ >= 0) {
        // Best-effort QUIT; if the helper has already crashed the
        // write fails silently and we fall through to SIGTERM below.
        (void)write_line("QUIT");
    }
    stop_reader_.store(true);
    if (in_fd_ >= 0)  { ::close(in_fd_);  in_fd_  = -1; }
    if (out_fd_ >= 0) { ::close(out_fd_); out_fd_ = -1; }
    if (reader_thread_.joinable()) reader_thread_.join();
    if (pid_ > 0) {
        for (int i = 0; i < 20; i++) {
            int status = 0;
            pid_t r = waitpid(pid_, &status, WNOHANG);
            if (r == pid_ || r < 0) { pid_ = -1; break; }
            struct timespec ts{0, 10 * 1000 * 1000};  // 10 ms
            nanosleep(&ts, nullptr);
        }
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            waitpid(pid_, nullptr, 0);
            pid_ = -1;
        }
    }
    on_status_ = nullptr;
}

void ChessnutBridge::request_connect() {
    write_line("INIT");
}

void ChessnutBridge::send_fen(const std::string& fen, bool force) {
    write_line(std::string(force ? "FEN_FORCE " : "FEN ") + fen);
}

void ChessnutBridge::send_led_hex(const std::string& bitmask_hex) {
    write_line("LED " + bitmask_hex);
}

bool ChessnutBridge::write_line(const std::string& line) {
    if (in_fd_ < 0) return false;
    std::lock_guard<std::mutex> lk(write_mu_);
    std::string buf = line + "\n";
    const char* p = buf.data();
    size_t left = buf.size();
    while (left > 0) {
        ssize_t n = ::write(in_fd_, p, left);
        if (n > 0) {
            p += n;
            left -= static_cast<size_t>(n);
        } else if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

void ChessnutBridge::reader_loop() {
    std::string buf;
    char chunk[512];
    while (!stop_reader_.load()) {
        struct pollfd pfd{};
        pfd.fd = out_fd_;
        pfd.events = POLLIN;
        int r = ::poll(&pfd, 1, 200);  // 200 ms tick — responsive shutdown
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;
        if (!(pfd.revents & POLLIN)) {
            if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) break;
            continue;
        }
        ssize_t n = ::read(out_fd_, chunk, sizeof(chunk));
        if (n <= 0) break;
        buf.append(chunk, chunk + n);

        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (on_status_) on_status_(line);
        }
    }
    // Surface a final DISCONNECTED so AppState can flip the toggle
    // off if the bridge died unexpectedly.
    if (!stop_reader_.load() && on_status_) {
        on_status_("DISCONNECTED");
    }
}

#endif  // !__EMSCRIPTEN__
