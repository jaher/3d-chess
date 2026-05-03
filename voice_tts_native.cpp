// Desktop TTS impl — shells out to Piper neural-TTS for natural-
// sounding voice. Same shape as the espeak-ng version this
// replaces (worker thread + queue + audio_play_pcm dispatch), but
// the synthesizer is the prebuilt `piper` binary fetched on first
// `make` into third_party/piper-bin/. Per-utterance flow:
//
//   parent: fork
//   child:  exec piper --model ... --output-raw --quiet
//           (text comes in on stdin, raw S16 PCM goes out on stdout)
//   parent: write text → close stdin → drain stdout PCM →
//           waitpid → audio_play_pcm
//
// Per-utterance subprocess startup adds ~80 ms latency before the
// first sample plays, but the move-animation arc already runs
// ~500 ms so it's masked. Voice quality matches the web build's
// browser speechSynthesis; far less robotic than the formant-synth
// espeak-ng path was.
//
// Paths to the binary and model are baked in at compile time via
// the Makefile's PIPER_BINARY_PATH / PIPER_MODEL_PATH defines.

#ifndef __EMSCRIPTEN__

#include "voice_tts.h"
#include "audio.h"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

#ifndef PIPER_BINARY_PATH
#  define PIPER_BINARY_PATH ""
#endif
#ifndef PIPER_MODEL_PATH
#  define PIPER_MODEL_PATH ""
#endif

std::mutex                g_init_mu;
std::atomic<bool>         g_inited{false};
std::atomic<bool>         g_init_failed{false};

std::thread               g_worker;
std::mutex                g_q_mu;
std::condition_variable   g_q_cv;
std::deque<std::string>   g_q;
std::atomic<bool>         g_worker_running{false};

// Drain a single Piper invocation: pipe text → child stdin, read
// raw S16 PCM → child stdout. Returns true if the child exited
// cleanly with PCM data; the caller is responsible for handing
// `out_pcm` to audio_play_pcm.
//
// Uses a writer thread so the parent can read stdout concurrently
// — without it, a long utterance can fill the child's stdout pipe
// buffer before we've finished writing stdin and the whole flow
// deadlocks.
bool synthesize_via_piper(const std::string& text,
                          std::vector<int16_t>* out_pcm) {
    int in_pipe[2];   // parent → child stdin
    int out_pipe[2];  // child stdout → parent
    // O_CLOEXEC is critical here. Without it, an unrelated fork+exec
    // anywhere else in the process (e.g. AI thread spawning
    // Stockfish moments after we kicked off piper) inherits these
    // pipe FDs. Stockfish's exec wouldn't close them, so when the
    // piper child exits its stdout pipe stays "open" via Stockfish's
    // copy — the parent's read() below blocks forever waiting for
    // an EOF that never comes. Symptom: "piper synth" log appears
    // but "piper done" never does. With O_CLOEXEC the pipe ends
    // close on every exec the parent does later, so EOF arrives
    // when piper exits as expected. dup2 in the child clears
    // CLOEXEC on the new FD numbers (0/1), so piper itself sees a
    // normal stdin/stdout.
    if (pipe2(in_pipe, O_CLOEXEC) < 0) return false;
    if (pipe2(out_pipe, O_CLOEXEC) < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return false;
    }
    if (pid == 0) {
        // Child: rewire stdin/stdout/stderr and exec piper.
        dup2(in_pipe[0],  STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execl(PIPER_BINARY_PATH, "piper",
              "--model", PIPER_MODEL_PATH,
              "--output-raw",
              "--quiet",
              static_cast<char*>(nullptr));
        _exit(127);  // exec failed
    }

    // Parent: close ends we won't use.
    close(in_pipe[0]);
    close(out_pipe[1]);

    // Writer thread — push the text into the child's stdin and
    // close. Closing signals end-of-input so piper finishes
    // synthesizing rather than waiting for more.
    int stdin_fd  = in_pipe[1];
    int stdout_fd = out_pipe[0];
    std::thread writer([stdin_fd, text]() {
        size_t total = 0;
        while (total < text.size()) {
            ssize_t w = write(stdin_fd, text.data() + total,
                              text.size() - total);
            if (w < 0) {
                if (errno == EINTR) continue;
                break;
            }
            total += static_cast<size_t>(w);
        }
        close(stdin_fd);
    });

    // Drain stdout into the PCM buffer. piper writes raw S16 LE
    // samples at the model's native rate (22050 Hz for
    // en_US-amy-medium, matching what audio.cpp opens).
    char buf[8192];
    while (true) {
        ssize_t n = read(stdout_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        const int16_t* samples = reinterpret_cast<const int16_t*>(buf);
        size_t sample_count = static_cast<size_t>(n) / sizeof(int16_t);
        out_pcm->insert(out_pcm->end(), samples, samples + sample_count);
    }
    close(stdout_fd);
    writer.join();

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void worker_loop() {
    while (g_worker_running.load()) {
        std::string text;
        {
            std::unique_lock<std::mutex> lk(g_q_mu);
            g_q_cv.wait(lk, [] {
                return !g_q.empty() || !g_worker_running.load();
            });
            if (!g_worker_running.load()) return;
            text = std::move(g_q.front());
            g_q.pop_front();
        }
        if (text.empty()) continue;

        std::fprintf(stderr,
            "[voice_tts] piper synth: \"%s\"\n", text.c_str());
        std::vector<int16_t> pcm;
        bool ok = synthesize_via_piper(text, &pcm);
        std::fprintf(stderr,
            "[voice_tts] piper %s — %zu samples\n",
            ok ? "done" : "failed", pcm.size());
        if (ok && !pcm.empty()) audio_play_pcm(std::move(pcm));
    }
}

bool path_executable(const char* p) {
    return p && *p && access(p, X_OK) == 0;
}
bool path_readable(const char* p) {
    return p && *p && access(p, R_OK) == 0;
}

}  // namespace

bool voice_tts_init(std::string& err_out) {
    std::lock_guard<std::mutex> lk(g_init_mu);
    if (g_inited.load()) return true;

    if (!path_executable(PIPER_BINARY_PATH)) {
        err_out = std::string("piper binary missing at ") +
                  PIPER_BINARY_PATH +
                  " — run `make fetch-piper-binary`";
        return false;
    }
    if (!path_readable(PIPER_MODEL_PATH)) {
        err_out = std::string("piper voice model missing at ") +
                  PIPER_MODEL_PATH +
                  " — run `make fetch-piper-model`";
        return false;
    }

    // Ignore SIGPIPE — without this, the parent dies if the child
    // closes its stdin before we finish writing (rare, but happens
    // when piper rejects a malformed input or runs out of resources).
    signal(SIGPIPE, SIG_IGN);

    std::fprintf(stderr,
        "voice_tts: piper initialised — binary=%s model=%s\n",
        PIPER_BINARY_PATH, PIPER_MODEL_PATH);

    g_worker_running.store(true);
    g_worker = std::thread(worker_loop);
    g_inited.store(true);
    return true;
}

void voice_tts_speak(const std::string& text) {
    if (text.empty()) return;
    if (!g_inited.load() && !g_init_failed.load()) {
        std::string err;
        if (!voice_tts_init(err)) {
            std::fprintf(stderr,
                "voice_tts: lazy init failed: %s\n", err.c_str());
            g_init_failed.store(true);
            return;
        }
    }
    if (!g_inited.load()) return;
    {
        std::lock_guard<std::mutex> lk(g_q_mu);
        // Drop in-flight backlog past a couple of utterances — the
        // user wants the latest move read, not a historical recap.
        if (g_q.size() >= 3) g_q.clear();
        g_q.push_back(text);
    }
    g_q_cv.notify_one();
}

void voice_tts_shutdown() {
    if (!g_inited.exchange(false)) return;
    g_worker_running.store(false);
    g_q_cv.notify_all();
    // Detach instead of join — a worker mid-piper-subprocess can't
    // be cancelled cleanly. Detaching hands the thread to the OS
    // reaper at process exit; the next dequeue iteration sees the
    // running-flag flipped and exits naturally for the idle case.
    if (g_worker.joinable()) g_worker.detach();
}

#endif  // !__EMSCRIPTEN__
