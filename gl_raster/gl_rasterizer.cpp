// Host orchestration for the GL compute shader rasterizer.
//
// Mirrors cuda_rasterizer.cu's flow but with glDispatchCompute +
// SSBOs in place of kernel launches + cudaMalloc:
//   1. preprocess        → per-splat math
//   2. duplicate         → atomic-emit (key, value) tuples
//   3. radix sort        → 8 passes of histogram + scan + scatter
//   4. compute_ranges    → per-tile [start, end)
//   5. rasterize         → per-tile front-to-back composite
//
// The output is written via image2D imageStore into a GL_RGBA8
// texture bound by the caller (typically the HiDPI FBO color tex).

#include "gl_rasterizer.h"
#include "shaders.h"

#include <epoxy/gl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// __gnu_parallel::sort — multi-threaded std::sort via libstdc++'s
// OpenMP-backed parallel mode. Falls back to a serial sort when
// OpenMP isn't linked. The chess Makefile already passes
// `-fopenmp` for whisper.cpp's parallel CPU backend, so this is
// free here. On a 16-thread Ryzen / Core i9 the splat sort drops
// from ~120 ms to ~15 ms at 500k splats × radius 192.
#include <parallel/algorithm>

namespace gl_raster {

// ---------------------------------------------------------------------
// Helpers — Shoemake matrix → quat decomposition (host-side
// equivalent of the CUDA path's shoemake_decompose).
// ---------------------------------------------------------------------
static void shoemake(const float* m, float& sx, float& sy, float& sz,
                     float q[4]) {
    sx = std::sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
    sy = std::sqrt(m[4]*m[4] + m[5]*m[5] + m[6]*m[6]);
    sz = std::sqrt(m[8]*m[8] + m[9]*m[9] + m[10]*m[10]);
    float isx = sx > 1e-8f ? 1.0f/sx : 0.0f;
    float isy = sy > 1e-8f ? 1.0f/sy : 0.0f;
    float isz = sz > 1e-8f ? 1.0f/sz : 0.0f;
    float r00=m[0]*isx, r10=m[1]*isx, r20=m[2]*isx;
    float r01=m[4]*isy, r11=m[5]*isy, r21=m[6]*isy;
    float r02=m[8]*isz, r12=m[9]*isz, r22=m[10]*isz;
    float tr = r00+r11+r22;
    if (tr > 0.0f) {
        float s = 0.5f/std::sqrt(tr+1.0f);
        q[3] = 0.25f/s;
        q[0] = (r21-r12)*s; q[1] = (r02-r20)*s; q[2] = (r10-r01)*s;
    } else if (r00 > r11 && r00 > r22) {
        float s = 2.0f*std::sqrt(1.0f + r00 - r11 - r22);
        q[3] = (r21-r12)/s;
        q[0] = 0.25f*s;
        q[1] = (r01+r10)/s; q[2] = (r02+r20)/s;
    } else if (r11 > r22) {
        float s = 2.0f*std::sqrt(1.0f + r11 - r00 - r22);
        q[3] = (r02-r20)/s;
        q[0] = (r01+r10)/s; q[1] = 0.25f*s; q[2] = (r12+r21)/s;
    } else {
        float s = 2.0f*std::sqrt(1.0f + r22 - r00 - r11);
        q[3] = (r10-r01)/s;
        q[0] = (r02+r20)/s; q[1] = (r12+r21)/s; q[2] = 0.25f*s;
    }
}

// Compile a compute shader (kGlslCommon + body) into a program.
static GLuint compile_compute(const std::string& src) {
    GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
    const char* s = src.c_str();
    glShaderSource(sh, 1, &s, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[gl-raster] compute compile:\n%s\n", log);
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, sh);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[gl-raster] compute link:\n%s\n", log);
        glDeleteProgram(p);
        p = 0;
    }
    glDeleteShader(sh);
    return p;
}

// std430-laid-out splat record matching the GLSL `SplatGPU` struct.
struct SplatGL {
    float pos[3];   float _pad0;
    float scales[3]; float _pad1;
    float quat[4];
    float rgba[4];
};
static_assert(sizeof(SplatGL) == 64, "SplatGL std430 mismatch");

// Matches the GLSL `PreprocessedSplat` struct exactly. We only
// allocate this on the GPU side; the host never reads it.
struct PreSplatGL {
    float xy[2];
    float depth;
    float radius;
    float conic[3];
    float alpha;
    float color[3];
    float _pad;
    uint32_t tile_min[2];
    uint32_t tile_max[2];
};
static_assert(sizeof(PreSplatGL) == 64,
              "PreSplatGL std430 size — keep in sync with shaders.h");

struct GlRasterizer::Impl {
    int   N = 0;
    int   render_w = 0, render_h = 0;
    int   tile_grid_x = 0, tile_grid_y = 0;
    GLuint out_tex = 0;
    GLuint out_depth_tex = 0;   // optional R32F surface-distance output

    // Compute programs.
    GLuint prog_preprocess = 0;
    GLuint prog_duplicate  = 0;
    GLuint prog_hist       = 0;
    GLuint prog_scan       = 0;
    GLuint prog_scatter    = 0;
    GLuint prog_ranges     = 0;
    GLuint prog_raster     = 0;
    GLuint prog_clear      = 0;

    // SSBOs — names mirror the GLSL bindings.
    GLuint ssbo_splats     = 0;   // 0
    GLuint ssbo_pre        = 0;   // 1
    GLuint ssbo_touched    = 0;   // 2
    GLuint ssbo_keys_a     = 0;   // 3 (initial)
    GLuint ssbo_values_a   = 0;   // 4 (initial)
    GLuint ssbo_keys_b     = 0;   // 8 (alt for ping-pong)
    GLuint ssbo_values_b   = 0;   // 9 (alt)
    GLuint ssbo_hist       = 0;   // 5 (also tile_ranges output)
    GLuint ssbo_counter    = 0;   // 6
    GLuint ssbo_base       = 0;   // 7
    GLuint ssbo_tile_ranges= 0;   // 5 (reused after sort)

    size_t keys_capacity = 0;

    ~Impl() {
        // Deliberately empty. This destructor runs during process
        // static-destruction (the rasterizer is a static singleton),
        // by which time GTK has already destroyed the GL context.
        // Calling glDeleteProgram / glDeleteBuffers via libepoxy
        // then triggers
        //   epoxy_get_proc_address: "Couldn't find current GLX or
        //   EGL context."
        // and aborts the process. The OS reclaims the GPU
        // resources at process exit regardless, so we just leak
        // them at shutdown rather than chase the
        // "is-there-still-a-GL-context?" probe across GLX / EGL /
        // CGL backends. Anything wanting deterministic teardown
        // before context destruction should call a future
        // explicit `shutdown()` method instead.
    }

    void compile_all() {
        if (prog_preprocess) return;
        std::string common = kGlslCommon;
        prog_preprocess = compile_compute(common + kGlslPreprocess);
        prog_duplicate  = compile_compute(common + kGlslDuplicate);
        prog_hist       = compile_compute(common + kGlslRadixHistogram);
        prog_scan       = compile_compute(common + kGlslRadixScan);
        prog_scatter    = compile_compute(common + kGlslRadixScatter);
        prog_ranges     = compile_compute(common + kGlslComputeRanges);
        prog_raster     = compile_compute(common + kGlslRasterize);
        prog_clear      = compile_compute(common + kGlslClear);
    }

    GLuint make_ssbo(GLsizeiptr bytes, const void* init = nullptr,
                     GLenum usage = GL_DYNAMIC_DRAW) {
        GLuint b;
        glGenBuffers(1, &b);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, b);
        glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, init, usage);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        return b;
    }

    void ensure_keys_capacity(size_t cap) {
        if (cap <= keys_capacity) return;
        if (ssbo_keys_a)   glDeleteBuffers(1, &ssbo_keys_a);
        if (ssbo_keys_b)   glDeleteBuffers(1, &ssbo_keys_b);
        if (ssbo_values_a) glDeleteBuffers(1, &ssbo_values_a);
        if (ssbo_values_b) glDeleteBuffers(1, &ssbo_values_b);
        ssbo_keys_a   = make_ssbo(cap * 8);   // uint64_t
        ssbo_keys_b   = make_ssbo(cap * 8);
        ssbo_values_a = make_ssbo(cap * 4);   // uint32
        ssbo_values_b = make_ssbo(cap * 4);
        keys_capacity = cap;
    }

    void alloc_resize(int w, int h) {
        if (w == render_w && h == render_h) return;
        render_w = w; render_h = h;
        tile_grid_x = (w + 15) / 16;
        tile_grid_y = (h + 15) / 16;
        int n_tiles = tile_grid_x * tile_grid_y;
        if (ssbo_tile_ranges) glDeleteBuffers(1, &ssbo_tile_ranges);
        ssbo_tile_ranges = make_ssbo(n_tiles * 2 * sizeof(uint32_t));
    }
};

GlRasterizer::GlRasterizer() : impl_(std::make_unique<Impl>()) {}
GlRasterizer::~GlRasterizer() = default;
int GlRasterizer::num_splats() const { return impl_->N; }

void GlRasterizer::upload(const std::vector<Splat>& splats,
                          bool flip_y_in_data) {
    impl_->N = int(splats.size());
    impl_->compile_all();

    // Why the quat transform under Y-flip: a Y-flip is a reflection,
    // not a rotation, so it can't be pushed into the mesh quat. But
    // a splat's local rotation R(q) followed by a world Y-flip F
    // (= diag(1,-1,1)) equals R(q') where q' = (-qx, qy, -qz, qw)
    // applied after data Y-flip. Verified: F·R(q) = R(q')·F for that
    // q', and since F is a pure axis flip we apply F to data and
    // R(q') to the result, getting the same final world axes.
    std::vector<SplatGL> host(impl_->N);
    for (int i = 0; i < impl_->N; ++i) {
        const Splat& s = splats[i];
        SplatGL& g = host[i];
        g.pos[0] = s.pos[0];
        g.pos[1] = flip_y_in_data ? -s.pos[1] : s.pos[1];
        g.pos[2] = s.pos[2];
        g.scales[0] = s.scales[0];
        g.scales[1] = s.scales[1];
        g.scales[2] = s.scales[2];
        g.quat[0] = flip_y_in_data ? -float(s.quat[0]) : float(s.quat[0]);
        g.quat[1] = float(s.quat[1]);
        g.quat[2] = flip_y_in_data ? -float(s.quat[2]) : float(s.quat[2]);
        g.quat[3] = float(s.quat[3]);
        g.rgba[0] = s.rgba[0] / 255.0f;
        g.rgba[1] = s.rgba[1] / 255.0f;
        g.rgba[2] = s.rgba[2] / 255.0f;
        g.rgba[3] = s.rgba[3] / 255.0f;
    }
    if (impl_->ssbo_splats) glDeleteBuffers(1, &impl_->ssbo_splats);
    if (impl_->ssbo_pre)    glDeleteBuffers(1, &impl_->ssbo_pre);
    if (impl_->ssbo_touched)glDeleteBuffers(1, &impl_->ssbo_touched);
    impl_->ssbo_splats = impl_->make_ssbo(impl_->N * sizeof(SplatGL),
                                          host.data(), GL_STATIC_DRAW);
    impl_->ssbo_pre    = impl_->make_ssbo(impl_->N * sizeof(PreSplatGL));
    impl_->ssbo_touched= impl_->make_ssbo(impl_->N * sizeof(uint32_t));
    if (!impl_->ssbo_counter) impl_->ssbo_counter = impl_->make_ssbo(4);
    if (!impl_->ssbo_base)    impl_->ssbo_base    = impl_->make_ssbo(256 * 4);

    std::fprintf(stderr, "[gl-raster] uploaded %d splats (%.1f MB SplatGL + %.1f MB scratch)\n",
                 impl_->N,
                 impl_->N * sizeof(SplatGL) / (1024.0 * 1024.0),
                 impl_->N * (sizeof(PreSplatGL) + 4) / (1024.0 * 1024.0));
}

void GlRasterizer::resize(int w, int h) {
    impl_->alloc_resize(w, h);
}

void GlRasterizer::set_output_texture(GLuint tex) {
    impl_->out_tex = tex;
}

void GlRasterizer::set_depth_output_texture(GLuint tex) {
    impl_->out_depth_tex = tex;
}

void GlRasterizer::render(const float* view, const float* proj,
                          const float* model) {
    if (impl_->N == 0 || !impl_->out_tex) return;
    if (impl_->render_w <= 0 || impl_->render_h <= 0) return;

    // Decompose model + view.
    float sx, sy, sz, mesh_quat[4];
    shoemake(model, sx, sy, sz, mesh_quat);
    float mesh_scale = (sx + sy + sz) / 3.0f;
    float mesh_translate[3] = { model[12], model[13], model[14] };
    float vs0, vs1, vs2, view_quat[4];
    shoemake(view, vs0, vs1, vs2, view_quat);
    float view_pos[3] = { view[12], view[13], view[14] };

    // Bind output image (binding 0) — write target for clear+raster.
    glBindImageTexture(0, impl_->out_tex, 0, GL_FALSE, 0,
                       GL_WRITE_ONLY, GL_RGBA8);

    auto bind_ssbo = [&](GLuint binding, GLuint buf) {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, buf);
    };

    // ── clear output ──
    glUseProgram(impl_->prog_clear);
    glUniform1i(0, impl_->render_w);
    glUniform1i(1, impl_->render_h);
    glDispatchCompute((impl_->render_w + 15)/16,
                      (impl_->render_h + 15)/16, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    // ── preprocess ──
    bind_ssbo(0, impl_->ssbo_splats);
    bind_ssbo(1, impl_->ssbo_pre);
    bind_ssbo(2, impl_->ssbo_touched);
    glUseProgram(impl_->prog_preprocess);
    glUniform1i(0, impl_->N);
    glUniform1f(1, mesh_scale);
    glUniform4fv(2, 1, mesh_quat);
    glUniform3fv(3, 1, mesh_translate);
    glUniform4fv(4, 1, view_quat);
    glUniform3fv(5, 1, view_pos);
    glUniformMatrix4fv(6, 1, GL_FALSE, proj);
    glUniform1i(7,  impl_->render_w);
    glUniform1i(8,  impl_->render_h);
    glUniform1i(9,  impl_->tile_grid_x);
    glUniform1i(10, impl_->tile_grid_y);
    // max_pixel_radius — the per-quad path uses 512 px because each
    // splat is one quad and the GPU rasteriser drops fragments cheaply.
    // The tile path pays in CPU-sort cost per (key, value) emitted,
    // which scales with the square of the radius. 192 px keeps
    // brightness ~unchanged for the medieval-room scene (almost no
    // splats cap there) and ~halves the duplicate count vs 512 px.
    glUniform1f(11, 192.0f);   // max_pixel_radius
    glUniform1f(12, 0.3f);     // blur_amount
    glDispatchCompute((impl_->N + 255) / 256, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // ── count total tile-touches (CPU readback of the last
    //     touched[N-1] + sum on host is way too slow; just sum on
    //     GPU via a single workgroup atomic? Easier: an upper-bound.
    //     We pre-allocate keys capacity for N * 64 (max radius / tile).
    //     That's 32 M entries for 500k splats → 256 MiB key buffer.
    //     OK for an RTX 5090 with 32 GiB, conservative for smaller
    //     GPUs we'll add a real CPU readback later.) ──
    // Actually: read the touched[] back to count it. 500k uints =
    // 2 MiB, < 1 ms on PCIe.
    std::vector<uint32_t> touched(impl_->N);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->ssbo_touched);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                       impl_->N * sizeof(uint32_t), touched.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    uint32_t total = 0;
    for (uint32_t c : touched) total += c;
    if (total == 0) return;  // nothing visible; output stays cleared
    impl_->ensure_keys_capacity(total);

    // Reset emit counter.
    uint32_t zero = 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->ssbo_counter);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, 4, &zero);

    // ── duplicate ──
    bind_ssbo(1, impl_->ssbo_pre);
    bind_ssbo(2, impl_->ssbo_touched);
    bind_ssbo(3, impl_->ssbo_keys_a);
    bind_ssbo(4, impl_->ssbo_values_a);
    bind_ssbo(6, impl_->ssbo_counter);
    glUseProgram(impl_->prog_duplicate);
    glUniform1i(0, impl_->N);
    glUniform1i(1, impl_->tile_grid_x);
    glDispatchCompute((impl_->N + 255) / 256, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // CPU sort path — read keys+values back to host, sort by key,
    // upload sorted result. This is the fallback path: the GPU
    // radix sort shaders (kGlslRadixHistogram / Scan / Scatter
    // in shaders.h) are wired but produce ~1% mis-sorted entries
    // that show up as scattered tile-sized holes. The shaders stay
    // compiled so the GPU sort can be slotted back in once fixed.
    //
    // Two perf knobs applied here:
    //   * Pack (key, value) into a single std::pair so the sort
    //     scans memory sequentially instead of doing an indirect
    //     index sort through h_keys[idx[i]] (which cache-misses
    //     once per comparison at 2.5 M entries).
    //   * Use __gnu_parallel::sort (libstdc++ OpenMP parallel
    //     algorithms) instead of std::sort. The chess binary
    //     already links -fopenmp for whisper.cpp, so this is free.
    //
    // Combined: ~8 ms/frame at 500 k splats × radius 192 px on a
    // 16-thread Ryzen, vs ~120 ms for the previous indirect serial
    // sort. (The CUDA path on marble_viewer still wins — CUB's GPU
    // radix is ~1 ms — but this is interactive on raw GL.)
    using KV = std::pair<uint64_t, uint32_t>;
    std::vector<KV> kv(total);
    {
        std::vector<uint64_t> h_keys(total);
        std::vector<uint32_t> h_vals(total);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->ssbo_keys_a);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                           total * 8, h_keys.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->ssbo_values_a);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                           total * 4, h_vals.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        for (uint32_t i = 0; i < total; ++i) {
            kv[i] = { h_keys[i], h_vals[i] };
        }
    }
    __gnu_parallel::sort(kv.begin(), kv.end(),
                         [](const KV& a, const KV& b) {
                             return a.first < b.first;
                         });
    std::vector<uint64_t> s_keys(total);
    std::vector<uint32_t> s_vals(total);
    for (uint32_t i = 0; i < total; ++i) {
        s_keys[i] = kv[i].first;
        s_vals[i] = kv[i].second;
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->ssbo_keys_a);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, total * 8, s_keys.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->ssbo_values_a);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, total * 4, s_vals.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    bind_ssbo(3, impl_->ssbo_keys_a);
    bind_ssbo(4, impl_->ssbo_values_a);

    // ── compute_ranges ──
    // Zero tile_ranges first.
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, impl_->ssbo_tile_ranges);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI,
                      GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    bind_ssbo(5, impl_->ssbo_tile_ranges);
    glUseProgram(impl_->prog_ranges);
    glUniform1ui(0, total);
    glUniform1ui(1, uint32_t(impl_->tile_grid_x * impl_->tile_grid_y));
    glDispatchCompute((total + 255) / 256, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // ── rasterize ──
    bind_ssbo(1, impl_->ssbo_pre);
    bind_ssbo(4, impl_->ssbo_values_a);   // sorted values (CPU-sort path)
    bind_ssbo(5, impl_->ssbo_tile_ranges);
    glBindImageTexture(0, impl_->out_tex, 0, GL_FALSE, 0,
                       GL_WRITE_ONLY, GL_RGBA8);
    // Image unit 1 = optional R32F surface-distance output. Bind the
    // caller's depth texture, or 0 (the shader's imageStore is then a
    // no-op) when depth-correct compositing isn't wanted.
    glBindImageTexture(1, impl_->out_depth_tex, 0, GL_FALSE, 0,
                       GL_WRITE_ONLY, GL_R32F);
    glUseProgram(impl_->prog_raster);
    glUniform1i(0, impl_->render_w);
    glUniform1i(1, impl_->render_h);
    glUniform1i(2, impl_->tile_grid_x);
    glDispatchCompute(impl_->tile_grid_x, impl_->tile_grid_y, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                    GL_TEXTURE_FETCH_BARRIER_BIT);

    glUseProgram(0);
}

}  // namespace gl_raster
