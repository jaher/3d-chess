#include "ai_player.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// FEN generation
// ---------------------------------------------------------------------------
char piece_to_fen(int type, bool is_white) {
    const char white_chars[] = "KQBNRP";
    const char black_chars[] = "kqbnrp";
    return is_white ? white_chars[type] : black_chars[type];
}

std::string board_to_fen(const BoardSquare board[8][8], bool white_turn,
                         bool wk_moved, bool bk_moved,
                         bool wra_moved, bool wrh_moved,
                         bool bra_moved, bool brh_moved) {
    std::string fen;

    // Ranks 8 to 1 (row 7 to 0), files a-h (internal col 7 to 0)
    for (int row = 7; row >= 0; row--) {
        int empty = 0;
        for (int col = 7; col >= 0; col--) {
            if (board[row][col].piece_type < 0) {
                empty++;
            } else {
                if (empty > 0) {
                    fen += std::to_string(empty);
                    empty = 0;
                }
                fen += piece_to_fen(board[row][col].piece_type,
                                    board[row][col].is_white);
            }
        }
        if (empty > 0) fen += std::to_string(empty);
        if (row > 0) fen += '/';
    }

    fen += white_turn ? " w " : " b ";

    // Castling rights
    std::string castling;
    if (!wk_moved && !wrh_moved) castling += 'K';
    if (!wk_moved && !wra_moved) castling += 'Q';
    if (!bk_moved && !brh_moved) castling += 'k';
    if (!bk_moved && !bra_moved) castling += 'q';
    if (castling.empty()) castling = "-";
    fen += castling + " - 0 1";

    return fen;
}

// ---------------------------------------------------------------------------
// UCI move helpers
// ---------------------------------------------------------------------------
int internal_col_to_file(int col) { return 7 - col; }
int file_to_internal_col(int file) { return 7 - file; }

std::string square_to_uci(int col, int row) {
    int file = internal_col_to_file(col);
    return std::string(1, static_cast<char>('a' + file)) +
           std::string(1, static_cast<char>('1' + row));
}

std::string move_to_uci(int from_col, int from_row, int to_col, int to_row) {
    return square_to_uci(from_col, from_row) + square_to_uci(to_col, to_row);
}

bool parse_uci_move(const std::string& move, int& from_col, int& from_row,
                    int& to_col, int& to_row) {
    if (move.size() < 4) return false;

    from_col = file_to_internal_col(move[0] - 'a');
    from_row = move[1] - '1';
    to_col   = file_to_internal_col(move[2] - 'a');
    to_row   = move[3] - '1';

    return from_col >= 0 && from_col < 8 &&
           from_row >= 0 && from_row < 8 &&
           to_col >= 0   && to_col < 8   &&
           to_row >= 0   && to_row < 8;
}

// ---------------------------------------------------------------------------
// Stockfish subprocess wrapper
// ---------------------------------------------------------------------------
// The web build (Emscripten) only needs the FEN/UCI helper functions above —
// the subprocess engine wrapper and the public ask_ai_move/stockfish_eval
// entry points are replaced by a JS bridge in web/ai_player_web.cpp. Define
// AI_PLAYER_HELPERS_ONLY when compiling for that target to skip the rest of
// this file.
#ifndef AI_PLAYER_HELPERS_ONLY

// Latched ELO override set by ai_player_set_elo(). -1 means "use the
// CHESS_AI_ELO env var fallback during handshake". Read by the engine's
// handshake() and by the public setter below. Must be accessed while
// holding g_engine_mu.
static int g_requested_elo = -1;

namespace {

int env_int(const char* name, int fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    long n = std::strtol(v, &end, 10);
    if (end == v) return fallback;
    return static_cast<int>(n);
}

// Portable pipe + FD_CLOEXEC. pipe2() is Linux-specific; macOS doesn't have
// it, so we fall back to pipe() + manual fcntl.
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

class StockfishEngine {
public:
    ~StockfishEngine() { stop(); }

    bool started() const { return pid_ > 0; }

    bool start() {
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
            // leave stderr attached to the parent's tty
            ::close(in_pipe[0]); ::close(in_pipe[1]);
            ::close(out_pipe[0]); ::close(out_pipe[1]);

            const char* env_path = std::getenv("CHESS_STOCKFISH_PATH");
            if (env_path && *env_path) {
                execl(env_path, env_path, (char*)nullptr);
            }
            execl("third_party/stockfish/src/stockfish",
                  "third_party/stockfish/src/stockfish", (char*)nullptr);
            execlp("stockfish", "stockfish", (char*)nullptr);
            _exit(127);
        }

        // parent
        ::close(in_pipe[0]);
        ::close(out_pipe[1]);
        in_fd_  = in_pipe[1];
        out_fd_ = out_pipe[0];
        pid_ = pid;

        // Non-blocking reads for poll-based wait_for_*
        int flags = fcntl(out_fd_, F_GETFL, 0);
        if (flags >= 0) fcntl(out_fd_, F_SETFL, flags | O_NONBLOCK);

        signal(SIGPIPE, SIG_IGN);

        if (!handshake()) {
            stop();
            return false;
        }
        std::fprintf(stderr, "Stockfish spawned pid=%d\n", pid_);
        return true;
    }

    void stop() {
        if (pid_ > 0) {
            if (in_fd_ >= 0) {
                (void)write_line("quit");
            }
        }
        if (in_fd_ >= 0)  { ::close(in_fd_);  in_fd_ = -1; }
        if (out_fd_ >= 0) { ::close(out_fd_); out_fd_ = -1; }
        if (pid_ > 0) {
            // brief grace period
            for (int i = 0; i < 20; i++) {
                int status = 0;
                pid_t r = waitpid(pid_, &status, WNOHANG);
                if (r == pid_ || r < 0) { pid_ = -1; break; }
                struct timespec ts{0, 10 * 1000 * 1000}; // 10ms
                nanosleep(&ts, nullptr);
            }
            if (pid_ > 0) {
                kill(pid_, SIGTERM);
                waitpid(pid_, nullptr, 0);
                pid_ = -1;
            }
        }
        read_buf_.clear();
    }

    std::string get_move(const std::string& fen, int movetime_ms) {
        if (!write_line("ucinewgame")) { stop(); return ""; }
        if (!write_line("isready"))    { stop(); return ""; }
        if (wait_for_contains("readyok", 2000).empty()) { stop(); return ""; }
        if (!write_line("position fen " + fen)) { stop(); return ""; }
        if (!write_line("go movetime " + std::to_string(movetime_ms))) {
            stop(); return "";
        }
        std::string line = wait_for_prefix("bestmove ", movetime_ms + 3000);
        if (line.empty()) { stop(); return ""; }

        // "bestmove e7e5 ponder d2d4" or "bestmove (none)" or "bestmove 0000"
        size_t p = line.find("bestmove ");
        if (p == std::string::npos) return "";
        std::string rest = line.substr(p + 9);
        // first token
        size_t sp = rest.find(' ');
        std::string mv = (sp == std::string::npos) ? rest : rest.substr(0, sp);
        if (mv == "(none)" || mv == "0000" || mv.size() < 4) return "";
        return mv.substr(0, 4);
    }

    // Returns centipawns from white's perspective, or INT_MIN on failure.
    int eval_position(const std::string& fen, int movetime_ms) {
        if (!write_line("ucinewgame")) { stop(); return INT_MIN; }
        if (!write_line("isready"))    { stop(); return INT_MIN; }
        if (wait_for_contains("readyok", 2000).empty()) { stop(); return INT_MIN; }
        if (!write_line("position fen " + fen)) { stop(); return INT_MIN; }
        if (!write_line("go movetime " + std::to_string(movetime_ms))) {
            stop(); return INT_MIN;
        }

        // Determine side to move from FEN (the field after the board).
        bool black_to_move = false;
        {
            size_t s = fen.find(' ');
            if (s != std::string::npos && s + 1 < fen.size() && fen[s + 1] == 'b')
                black_to_move = true;
        }

        int best_score = 0;
        int best_depth = -1;
        bool got_any = false;

        int deadline_budget = movetime_ms + 3000;
        while (true) {
            std::string line = read_line(deadline_budget);
            if (line.empty()) { stop(); return INT_MIN; }
            if (line.rfind("bestmove ", 0) == 0) break;

            if (line.rfind("info ", 0) != 0) continue;

            // Parse "... depth N ... score cp X" or "score mate X"
            int depth = -1;
            int score = 0;
            bool have_score = false;
            std::istringstream iss(line);
            std::string tok;
            while (iss >> tok) {
                if (tok == "depth") {
                    iss >> depth;
                } else if (tok == "score") {
                    std::string kind;
                    iss >> kind;
                    int n = 0;
                    iss >> n;
                    if (kind == "cp") {
                        score = n;
                        have_score = true;
                    } else if (kind == "mate") {
                        int absn = n < 0 ? -n : n;
                        score = (n >= 0 ? 1 : -1) * (30000 - absn);
                        have_score = true;
                    }
                }
            }
            if (have_score && depth >= best_depth) {
                best_score = score;
                best_depth = depth;
                got_any = true;
            }
        }

        if (!got_any) return 0;
        if (black_to_move) best_score = -best_score;
        return best_score;
    }

private:
    pid_t pid_ = -1;
    int in_fd_ = -1;
    int out_fd_ = -1;
    std::string read_buf_;

    bool write_line(const std::string& s) {
        if (in_fd_ < 0) return false;
        std::string out = s + "\n";
        const char* p = out.data();
        size_t left = out.size();
        while (left > 0) {
            ssize_t n = ::write(in_fd_, p, left);
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            p += n;
            left -= static_cast<size_t>(n);
        }
        return true;
    }

    // Returns the first complete line read from stdout, waiting up to
    // timeout_ms. Returns "" on error/EOF/timeout.
    std::string read_line(int timeout_ms) {
        while (true) {
            size_t nl = read_buf_.find('\n');
            if (nl != std::string::npos) {
                std::string line = read_buf_.substr(0, nl);
                read_buf_.erase(0, nl + 1);
                // trim \r
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return line;
            }
            if (out_fd_ < 0) return "";

            struct pollfd pfd;
            pfd.fd = out_fd_;
            pfd.events = POLLIN;
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr <= 0) return ""; // timeout or error
            if (!(pfd.revents & POLLIN)) return "";

            char buf[4096];
            ssize_t n = ::read(out_fd_, buf, sizeof(buf));
            if (n <= 0) return "";
            read_buf_.append(buf, static_cast<size_t>(n));
        }
    }

    std::string wait_for_contains(const std::string& needle, int timeout_ms) {
        while (true) {
            std::string line = read_line(timeout_ms);
            if (line.empty()) return "";
            if (line.find(needle) != std::string::npos) return line;
        }
    }

    std::string wait_for_prefix(const std::string& prefix, int timeout_ms) {
        while (true) {
            std::string line = read_line(timeout_ms);
            if (line.empty()) return "";
            if (line.rfind(prefix, 0) == 0) return line;
        }
    }

    bool handshake() {
        if (!write_line("uci")) return false;
        if (wait_for_contains("uciok", 3000).empty()) return false;
        int elo = g_requested_elo > 0
            ? g_requested_elo
            : env_int("CHESS_AI_ELO", 1400);
        if (!apply_elo(elo)) return false;
        if (!write_line("isready")) return false;
        if (wait_for_contains("readyok", 3000).empty()) return false;
        return true;
    }

    // Stockfish's UCI_Elo is only documented-accurate down to 1320.
    // Below that we fall back to the Skill Level option (0..20),
    // which Stockfish docs describe as roughly covering 800..2800 in
    // playing strength. The crossover at 1320 is where UCI_Elo's
    // floor starts and Skill Level ~12 (per community tuning); the
    // two settings are mutually exclusive, so whichever path we
    // take, we also explicitly turn off the other.
    bool apply_elo(int elo) {
        if (elo >= 1320) {
            if (!write_line("setoption name Skill Level value 20"))
                return false;
            if (!write_line("setoption name UCI_LimitStrength value true"))
                return false;
            if (!write_line("setoption name UCI_Elo value " +
                            std::to_string(elo)))
                return false;
        } else {
            // Map elo ∈ [800, 1320) → skill ∈ [0, 12].
            int skill = (elo - 800) * 12 / (1320 - 800);
            if (skill < 0) skill = 0;
            if (skill > 19) skill = 19;
            if (!write_line("setoption name UCI_LimitStrength value false"))
                return false;
            if (!write_line("setoption name Skill Level value " +
                            std::to_string(skill)))
                return false;
        }
        return true;
    }

public:
    // Send an ELO update to an already-running engine. Called by
    // ai_player_set_elo() with g_engine_mu held.
    bool set_elo(int elo) { return apply_elo(elo); }
};

std::mutex g_engine_mu;
std::unique_ptr<StockfishEngine> g_engine;
bool g_atexit_registered = false;

void ai_player_shutdown() {
    std::lock_guard<std::mutex> lk(g_engine_mu);
    g_engine.reset();
}

// Must be called with g_engine_mu held.
StockfishEngine* get_engine_locked() {
    if (!g_atexit_registered) {
        std::atexit(ai_player_shutdown);
        g_atexit_registered = true;
    }
    if (g_engine && g_engine->started()) return g_engine.get();
    g_engine = std::make_unique<StockfishEngine>();
    if (!g_engine->start()) {
        g_engine.reset();
        return nullptr;
    }
    return g_engine.get();
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
std::string ask_ai_move(const std::string& fen) {
    std::lock_guard<std::mutex> lk(g_engine_mu);
    StockfishEngine* eng = get_engine_locked();
    if (!eng) {
        std::fprintf(stderr, "Stockfish not available; falling back to random move\n");
        return "";
    }
    int movetime = env_int("CHESS_AI_MOVETIME_MS", 800);
    std::string uci = eng->get_move(fen, movetime);
    if (uci.empty()) {
        std::fprintf(stderr, "Stockfish produced no move; will fall back\n");
    }
    return uci;
}

int stockfish_eval(const std::string& fen, int movetime_ms) {
    std::lock_guard<std::mutex> lk(g_engine_mu);
    StockfishEngine* eng = get_engine_locked();
    if (!eng) return INT_MIN;
    return eng->eval_position(fen, movetime_ms);
}

void ai_player_set_elo(int elo) {
    if (elo < 1320) elo = 1320;
    if (elo > 3190) elo = 3190;
    std::lock_guard<std::mutex> lk(g_engine_mu);
    g_requested_elo = elo;
    // If the engine is already spawned, send setoption immediately. If
    // not, the new value will be picked up during the first handshake.
    if (g_engine && g_engine->started()) {
        g_engine->set_elo(elo);
    }
}

#endif // !AI_PLAYER_HELPERS_ONLY
