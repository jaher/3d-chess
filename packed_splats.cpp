#include "packed_splats.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#ifndef __EMSCRIPTEN__
// Worker-thread async sort uses these std::* threading primitives;
// Emscripten without -pthread doesn't have them, and turning pthreads
// on for the web build requires SharedArrayBuffer + COOP/COEP headers.
// Web takes the synchronous-sort path further down instead.
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#endif

namespace {

// ---------------------------------------------------------------------------
// Parallel LSD radix sort
// ---------------------------------------------------------------------------
//
// Sort an array of (depth, index) pairs back-to-front (largest depth
// first) by repeatedly bucketing on 8-bit slices of a depth-derived
// uint32 key. With 1.9 M splats and 8 threads on a modern CPU this
// completes in ~10–15 ms — comfortably within one render frame, so
// the ordering texture stays current frame-to-frame and Spark's full
// α-stretch path renders without "stale-order cloud" artefacts.

// Encode a float so unsigned integer ascending order corresponds to
// FLOAT DESCENDING order (largest depth first). For non-negative
// floats the bits invert; for negative floats only the sign bit is
// cleared. Both branches preserve total ordering and never produce
// the same key for distinct inputs.
inline uint32_t depth_to_desc_key(float f) {
    uint32_t b;
    std::memcpy(&b, &f, sizeof(b));
    return (b & 0x80000000u) ? (b & 0x7FFFFFFFu) : ~b;
}

// One LSD radix-sort pass: for each entry, slice the key at
// `byte_shift` and route into a per-bucket bin. Histograms are
// computed in parallel chunks then summed; the scatter phase is
// inherently sequential because of the prefix sums.
struct RadixPass {
    static constexpr int RADIX = 256;
    std::vector<uint64_t>* in;
    std::vector<uint64_t>* out;
    int                    byte_shift;

    void run(int num_threads) {
        const size_t n = in->size();
        if (n == 0) return;
        // Per-thread histograms (single bucket array on Emscripten,
        // since num_threads is forced to 1 there).
        std::vector<std::array<uint32_t, RADIX>> hist(num_threads);
        for (auto& h : hist) h.fill(0);
        const size_t chunk = (n + num_threads - 1) / num_threads;
#ifndef __EMSCRIPTEN__
        std::vector<std::future<void>> jobs;
        jobs.reserve(num_threads);
        for (int t = 0; t < num_threads; ++t) {
            jobs.push_back(std::async(std::launch::async, [&, t] {
                size_t lo = t * chunk;
                size_t hi = std::min(n, lo + chunk);
                auto& h = hist[t];
                for (size_t i = lo; i < hi; ++i) {
                    uint32_t key = static_cast<uint32_t>((*in)[i] >> 32);
                    uint8_t  b   = static_cast<uint8_t>((key >> byte_shift) & 0xFFu);
                    ++h[b];
                }
            }));
        }
        for (auto& j : jobs) j.wait();
#else
        // Synchronous histogram pass — single thread on web.
        auto& h = hist[0];
        for (size_t i = 0; i < n; ++i) {
            uint32_t key = static_cast<uint32_t>((*in)[i] >> 32);
            uint8_t  b   = static_cast<uint8_t>((key >> byte_shift) & 0xFFu);
            ++h[b];
        }
        (void)chunk;
#endif

        // Combine histograms and compute exclusive prefix sums.
        std::array<uint32_t, RADIX> total{};
        for (int t = 0; t < num_threads; ++t)
            for (int k = 0; k < RADIX; ++k)
                total[k] += hist[t][k];
        std::array<uint32_t, RADIX> offsets{};
        uint32_t acc = 0;
        for (int k = 0; k < RADIX; ++k) {
            offsets[k] = acc;
            acc += total[k];
        }

        // Sequential scatter — order within a bin is preserved
        // (LSD-radix's stability guarantee).
        for (size_t i = 0; i < n; ++i) {
            uint32_t key = static_cast<uint32_t>((*in)[i] >> 32);
            uint8_t  b   = static_cast<uint8_t>((key >> byte_shift) & 0xFFu);
            (*out)[offsets[b]++] = (*in)[i];
        }
    }
};

void parallel_radix_sort_descending_by_depth(
        std::vector<uint32_t>& idx_out,
        const std::vector<float>& depth) {
    const size_t n = depth.size();
    idx_out.resize(n);
    if (n == 0) return;

    // Pack (descending-uint key) << 32 | original index.
    std::vector<uint64_t> packed(n), scratch(n);
#ifdef __EMSCRIPTEN__
    // No std::thread / std::async on the default Emscripten build —
    // run the radix sort single-threaded. With ~30k splats (chess's
    // packed_splat_500k.spz tier or the panorama-only fallback in
    // practice) this still finishes well under one frame.
    const int num_threads = 1;
    for (size_t i = 0; i < n; ++i) {
        uint64_t key = depth_to_desc_key(depth[i]);
        packed[i] = (key << 32) | static_cast<uint32_t>(i);
    }
#else
    const int num_threads = std::max(2,
        std::min(8, static_cast<int>(std::thread::hardware_concurrency())));
    {
        const size_t chunk = (n + num_threads - 1) / num_threads;
        std::vector<std::future<void>> jobs;
        jobs.reserve(num_threads);
        for (int t = 0; t < num_threads; ++t) {
            jobs.push_back(std::async(std::launch::async, [&, t] {
                size_t lo = t * chunk;
                size_t hi = std::min(n, lo + chunk);
                for (size_t i = lo; i < hi; ++i) {
                    uint64_t key = depth_to_desc_key(depth[i]);
                    packed[i] = (key << 32) | static_cast<uint32_t>(i);
                }
            }));
        }
        for (auto& j : jobs) j.wait();
    }
#endif

    // Four 8-bit LSD passes over the upper 32-bit key.
    RadixPass p;
    p.in = &packed; p.out = &scratch; p.byte_shift =  0; p.run(num_threads);
    p.in = &scratch; p.out = &packed; p.byte_shift =  8; p.run(num_threads);
    p.in = &packed; p.out = &scratch; p.byte_shift = 16; p.run(num_threads);
    p.in = &scratch; p.out = &packed; p.byte_shift = 24; p.run(num_threads);

#ifdef __EMSCRIPTEN__
    for (size_t i = 0; i < n; ++i) {
        idx_out[i] = static_cast<uint32_t>(packed[i] & 0xFFFFFFFFu);
    }
#else
    {
        const size_t chunk = (n + num_threads - 1) / num_threads;
        std::vector<std::future<void>> jobs;
        jobs.reserve(num_threads);
        for (int t = 0; t < num_threads; ++t) {
            jobs.push_back(std::async(std::launch::async, [&, t] {
                size_t lo = t * chunk;
                size_t hi = std::min(n, lo + chunk);
                for (size_t i = lo; i < hi; ++i) {
                    idx_out[i] = static_cast<uint32_t>(packed[i] & 0xFFFFFFFFu);
                }
            }));
        }
        for (auto& j : jobs) j.wait();
    }
#endif
}

}  // namespace

// IEEE 754 binary32 → binary16 conversion. Same algorithm three.js's
// THREE.MathUtils.toHalfFloat uses (handles subnormals, infinities,
// NaN); we implement it ourselves so the C++ side doesn't depend on
// any half-float library. Output is a 16-bit unsigned value packed
// into the low half of a uint32_t for downstream packHalf2x16-style
// concatenation. Exposed via packed_splats.h so the dump path can do
// a half-float roundtrip when emulating Spark's PackedSplats format.
uint16_t to_half(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    uint32_t sign     = (bits >> 16) & 0x8000u;
    int32_t  exp      = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFFu;
    if (exp >= 31) {
        // Inf or NaN.
        return static_cast<uint16_t>(sign | 0x7C00u
                                     | (mantissa ? 0x200u : 0u));
    }
    // Round-to-nearest, ties-to-even (IEEE 754 default — what JS's
    // Float16Array uses, so a round-trip through to_half/from_half
    // matches Spark's PackedSplats values exactly). The simple
    // "round-half-up" the previous version used disagrees with Spark
    // on values that fall exactly on a half-float midpoint, e.g.
    // 7458 / 4096 = 1.820800781… (the 500k splat #2's pos.z).
    auto round_to_even_drop13 = [](uint32_t m) -> uint32_t {
        const uint32_t round_bit = m & 0x1000u;             // bit 12
        const uint32_t sticky    = m & 0x0FFFu;             // bits 0..11
        const uint32_t lsb_after = m & 0x2000u;             // bit 13 (LSB of result)
        return (round_bit && (sticky || lsb_after)) ? m + 0x2000u : m;
    };
    if (exp <= 0) {
        if (exp < -10) {
            // Underflows to zero.
            return static_cast<uint16_t>(sign);
        }
        // Subnormal — pack into a 10-bit mantissa with implicit shift.
        mantissa = (mantissa | 0x800000u) >> (1 - exp);
        mantissa = round_to_even_drop13(mantissa);
        return static_cast<uint16_t>(sign | (mantissa >> 13));
    }
    // Normal half range.
    {
        uint32_t rounded = round_to_even_drop13(mantissa);
        if (rounded & 0x800000u) {
            // Mantissa rolled past 23 bits — bump exponent.
            rounded = 0;
            ++exp;
            if (exp >= 31) {
                return static_cast<uint16_t>(sign | 0x7C00u);
            }
        }
        mantissa = rounded;
    }
    return static_cast<uint16_t>(sign
                                 | (static_cast<uint32_t>(exp) << 10)
                                 | (mantissa >> 13));
}

// IEEE 754 binary16 → binary32 conversion (inverse of to_half), so the
// dump path can simulate Spark's PackedSplats half-float roundtrip
// for centers, opacity, and color when comparing dump files.
float from_half(uint16_t h) {
    uint32_t sign = (static_cast<uint32_t>(h) & 0x8000u) << 16;
    int32_t  exp  = (static_cast<int32_t>(h) >> 10) & 0x1F;
    uint32_t mant = static_cast<uint32_t>(h) & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            // Subnormal half — renormalize as a normal float32.
            while (!(mant & 0x400u)) {
                mant <<= 1;
                --exp;
            }
            ++exp;
            mant &= ~0x400u;
            uint32_t exp_full = static_cast<uint32_t>(exp + 127 - 15);
            bits = sign | (exp_full << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        // Inf / NaN.
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        uint32_t exp_full = static_cast<uint32_t>(exp + 127 - 15);
        bits = sign | (exp_full << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

namespace {

// Equivalent to GLSL packHalf2x16: packs (x, y) into a single uint32
// with x in the low 16 bits and y in the high 16 bits.
uint32_t pack_half2(float x, float y) {
    uint32_t hx = to_half(x);
    uint32_t hy = to_half(y);
    return hx | (hy << 16);
}

// Pick a near-square 2D layout for N elements such that width is a
// power of two no smaller than 1024. This keeps `width × height` ≥ N
// and lets the vertex shader compute coords with a couple of
// integer ops.
void choose_layout(int n, int& width, int& height) {
    width = 1024;
    while (n > width * width) width *= 2;
    if (width > 8192) width = 8192;          // GL_MAX_TEXTURE_SIZE floor
    height = (n + width - 1) / width;
    if (height < 1) height = 1;
}

}  // namespace


void packed_splats_upload(PackedSplats& ps,
                          const std::vector<Splat>& splats) {
    ps.count = static_cast<int>(splats.size());
    if (ps.count == 0) return;
    choose_layout(ps.count, ps.tex_width, ps.tex_height);
    const int W = ps.tex_width, H = ps.tex_height;
    const int total = W * H;

    // Build texA + texB on the CPU; pad unused trailing texels with
    // zeros so the GPU texture has well-defined values for any
    // ordering-texture entries beyond `count` (we mark those with
    // 0xFFFFFFFF, but defensively zero them anyway).
    std::vector<uint32_t> a(static_cast<size_t>(total) * 4, 0u);
    std::vector<uint32_t> b(static_cast<size_t>(total) * 4, 0u);
    for (int i = 0; i < ps.count; ++i) {
        const Splat& s = splats[i];
        // log-encode the per-axis scales with a floor so log doesn't
        // blow up on degenerate zeros; the floor matches the floor
        // used to harden tiny splats elsewhere.
        const float ls = std::log(std::max(1e-6f, s.scales[0]));
        const float lt = std::log(std::max(1e-6f, s.scales[1]));
        const float lu = std::log(std::max(1e-6f, s.scales[2]));
        const float ralpha = s.rgba[3] / 255.0f;
        const float rR = s.rgba[0] / 255.0f;
        const float rG = s.rgba[1] / 255.0f;
        const float rB = s.rgba[2] / 255.0f;

        // texA layout: (x|y), (z|qx), (qy|qz), (alpha|0)
        const size_t ai = static_cast<size_t>(i) * 4;
        a[ai + 0] = pack_half2(s.pos[0], s.pos[1]);
        a[ai + 1] = pack_half2(s.pos[2], static_cast<float>(s.quat[0]));
        a[ai + 2] = pack_half2(static_cast<float>(s.quat[1]),
                               static_cast<float>(s.quat[2]));
        a[ai + 3] = pack_half2(ralpha,    0.0f);

        // texB layout: (r|g), (b|log_sx), (log_sy|log_sz), (qw|0)
        b[ai + 0] = pack_half2(rR, rG);
        b[ai + 1] = pack_half2(rB, ls);
        b[ai + 2] = pack_half2(lt, lu);
        b[ai + 3] = pack_half2(static_cast<float>(s.quat[3]), 0.0f);
    }

    auto upload_uvec4_tex = [&](GLuint& tex, const std::vector<uint32_t>& data) {
        if (tex) glDeleteTextures(1, &tex);
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, W, H, 0,
                     GL_RGBA_INTEGER, GL_UNSIGNED_INT, data.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    };
    upload_uvec4_tex(ps.texA, a);
    upload_uvec4_tex(ps.texB, b);

    // Ordering texture — identity at first; sort_and_upload replaces
    // it with sorted indices each camera-changing frame.
    std::vector<uint32_t> identity(static_cast<size_t>(total),
                                   0xFFFFFFFFu);
    for (int i = 0; i < ps.count; ++i) {
        identity[i] = static_cast<uint32_t>(i);
    }
    if (ps.texOrder) glDeleteTextures(1, &ps.texOrder);
    glGenTextures(1, &ps.texOrder);
    glBindTexture(GL_TEXTURE_2D, ps.texOrder);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, W, H, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_INT, identity.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    std::fprintf(stderr,
        "[packed-splats] uploaded %d splats into %d×%d textures "
        "(%.1f MB splat data + %.1f MB ordering)\n",
        ps.count, W, H,
        2.0 * total * 16.0 / 1024.0 / 1024.0,
        total * 4.0 / 1024.0 / 1024.0);
}


// ---------------------------------------------------------------------------
// Async sort worker
// ---------------------------------------------------------------------------
//
// Spark does its back-to-front sort in a Web Worker so the render loop never
// stalls, and tolerates a stale-but-correct ordering texture for a couple of
// frames after a sudden camera move. We mirror that with a single std::thread
// that owns its own scratch buffers; the GTK render thread queues work via
// `request_sort()` and consumes finished orderings via `poll_ready()`.
//
// Web takes the synchronous path inside packed_splats_sort_and_upload below.
// std::thread isn't available on the default Emscripten build (would need
// pthread support + COOP/COEP headers), so the per-frame radix sort runs
// inline on the render thread there.
namespace {

struct SortJob {
    float cam_pos[3];   // world-space camera position (for radial sort)
    float cam_dir[3];   // unused for sort, kept for movement gating
    float model[16];
};

#ifndef __EMSCRIPTEN__
class SortWorker {
public:
    void start(const std::vector<Splat>* source) {
        std::lock_guard<std::mutex> lk(mu_);
        if (running_) return;
        source_ = source;
        const int n = static_cast<int>(source->size());
        idx_.resize(n);
        depth_.resize(n);
        for (int i = 0; i < n; ++i) idx_[i] = static_cast<uint32_t>(i);
        running_ = true;
        thread_ = std::thread(&SortWorker::run, this);
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            running_ = false;
            cv_.notify_all();
        }
        if (thread_.joinable()) thread_.join();
    }
    ~SortWorker() { stop(); }

    // Queue a fresh sort. If a sort is already in flight, REPLACE its
    // pending input — we only care about the most recent camera state.
    void request(const SortJob& job) {
        std::lock_guard<std::mutex> lk(mu_);
        pending_job_ = job;
        have_pending_ = true;
        cv_.notify_one();
    }

    // Pull the latest completed ordering. Returns nullptr if none is
    // available. Caller must call before requesting a new one (the
    // worker overwrites once consumed).
    std::vector<uint32_t>* poll_ready() {
        std::lock_guard<std::mutex> lk(mu_);
        if (!ready_) return nullptr;
        ready_ = false;
        return &finished_idx_;
    }

private:
    void run() {
        while (true) {
            SortJob job;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&]{ return !running_ || have_pending_; });
                if (!running_) return;
                job = pending_job_;
                have_pending_ = false;
            }
            const int n = static_cast<int>(idx_.size());
            const auto& src = *source_;
            // Compute squared radial distance from camera to each
            // splat's world-space centre. Squared is fine because the
            // sort only needs ordering, and avoiding sqrt saves a few
            // ms on big splat counts. Bigger value = farther from
            // camera = back of the queue.
            for (int i = 0; i < n; ++i) {
                const Splat& s = src[i];
                float wx = s.pos[0]*job.model[0] + s.pos[1]*job.model[4]
                         + s.pos[2]*job.model[8] + job.model[12];
                float wy = s.pos[0]*job.model[1] + s.pos[1]*job.model[5]
                         + s.pos[2]*job.model[9] + job.model[13];
                float wz = s.pos[0]*job.model[2] + s.pos[1]*job.model[6]
                         + s.pos[2]*job.model[10]+ job.model[14];
                float dx = wx - job.cam_pos[0];
                float dy = wy - job.cam_pos[1];
                float dz = wz - job.cam_pos[2];
                depth_[i] = dx*dx + dy*dy + dz*dz;
            }
            // Parallel LSD radix sort — completes in ~10–15 ms on
            // 1.9 M splats vs std::sort's 50–100 ms, so the
            // ordering texture stays current frame-to-frame.
            parallel_radix_sort_descending_by_depth(idx_, depth_);
            {
                std::lock_guard<std::mutex> lk(mu_);
                finished_idx_ = idx_;
                ready_ = true;
            }
        }
    }

    std::thread             thread_;
    std::mutex              mu_;
    std::condition_variable cv_;
    bool                    running_      = false;
    bool                    have_pending_ = false;
    bool                    ready_        = false;
    SortJob                 pending_job_{};
    std::vector<uint32_t>   idx_;
    std::vector<float>      depth_;
    std::vector<uint32_t>   finished_idx_;
    const std::vector<Splat>* source_ = nullptr;
};

static SortWorker g_worker;
#endif  // !__EMSCRIPTEN__

static float      g_last_dir[3]    = {2.0f, 2.0f, 2.0f};
static bool       g_first_request  = true;

}  // namespace


#ifdef __EMSCRIPTEN__
// Helper used by the inline (web) sort path: compute squared-radial
// depth per splat and run the radix sort, returning a fresh ordering
// of length `count`. Desktop has its own analogous code in
// SortWorker::run() so this lives behind the same gate to keep
// the desktop build's -Wunused-function happy.
static void compute_radial_ordering(const std::vector<Splat>& source,
                                    const float cam_pos[3],
                                    const float model[16],
                                    std::vector<uint32_t>& idx_out) {
    const int n = static_cast<int>(source.size());
    std::vector<float> depth(n);
    for (int i = 0; i < n; ++i) {
        const Splat& s = source[i];
        float wx = s.pos[0]*model[0] + s.pos[1]*model[4]
                 + s.pos[2]*model[8] + model[12];
        float wy = s.pos[0]*model[1] + s.pos[1]*model[5]
                 + s.pos[2]*model[9] + model[13];
        float wz = s.pos[0]*model[2] + s.pos[1]*model[6]
                 + s.pos[2]*model[10]+ model[14];
        float dx = wx - cam_pos[0];
        float dy = wy - cam_pos[1];
        float dz = wz - cam_pos[2];
        depth[i] = dx*dx + dy*dy + dz*dz;
    }
    parallel_radix_sort_descending_by_depth(idx_out, depth);
}
#endif  // __EMSCRIPTEN__

static void upload_ordering(PackedSplats& ps,
                            const std::vector<uint32_t>& fresh) {
    const int total = ps.tex_width * ps.tex_height;
    std::vector<uint32_t> dst(static_cast<size_t>(total), 0xFFFFFFFFu);
    std::memcpy(dst.data(), fresh.data(),
                static_cast<size_t>(ps.count) * sizeof(uint32_t));
    glBindTexture(GL_TEXTURE_2D, ps.texOrder);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0,
                    0, 0, ps.tex_width, ps.tex_height,
                    GL_RED_INTEGER, GL_UNSIGNED_INT, dst.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}


void packed_splats_sort_and_upload(PackedSplats& ps,
                                   const float cam_pos[3],
                                   const float cam_dir[3],
                                   const float model[16],
                                   const std::vector<Splat>& source,
                                   float cos_threshold) {
    if (ps.count == 0) return;

#ifndef __EMSCRIPTEN__
    // Desktop: async worker thread.
    static const std::vector<Splat>* started_for       = nullptr;
    static size_t                    started_for_size  = 0;
    if (started_for != &source || started_for_size != source.size()) {
        g_worker.stop();
        g_worker.start(&source);
        started_for      = &source;
        started_for_size = source.size();
        g_first_request  = true;
        g_last_dir[0] = g_last_dir[1] = g_last_dir[2] = 2.0f;
    }

    bool queue = g_first_request;
    if (!queue) {
        float d = cam_dir[0] * g_last_dir[0]
                + cam_dir[1] * g_last_dir[1]
                + cam_dir[2] * g_last_dir[2];
        if (d <= cos_threshold) queue = true;
    }
    if (queue) {
        SortJob job;
        std::memcpy(job.cam_pos, cam_pos, sizeof(job.cam_pos));
        std::memcpy(job.cam_dir, cam_dir, sizeof(job.cam_dir));
        std::memcpy(job.model,   model,   sizeof(job.model));
        g_worker.request(job);
        g_last_dir[0] = cam_dir[0];
        g_last_dir[1] = cam_dir[1];
        g_last_dir[2] = cam_dir[2];
        g_first_request = false;
    }

    if (auto* fresh = g_worker.poll_ready()) {
        upload_ordering(ps, *fresh);
    }
#else
    // Web: synchronous radix sort inline on the render thread. We
    // still apply Spark's "did the camera move enough?" gate so we
    // don't burn frame time re-sorting the same ordering. With
    // ~30k–100k splats on the chess panorama-room tier, the
    // single-threaded radix sort completes in a few ms.
    bool sort_now = g_first_request;
    if (!sort_now) {
        float d = cam_dir[0] * g_last_dir[0]
                + cam_dir[1] * g_last_dir[1]
                + cam_dir[2] * g_last_dir[2];
        if (d <= cos_threshold) sort_now = true;
    }
    if (sort_now) {
        std::vector<uint32_t> idx;
        compute_radial_ordering(source, cam_pos, model, idx);
        upload_ordering(ps, idx);
        g_last_dir[0] = cam_dir[0];
        g_last_dir[1] = cam_dir[1];
        g_last_dir[2] = cam_dir[2];
        g_first_request = false;
    }
#endif
}
