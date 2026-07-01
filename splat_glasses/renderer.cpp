/*
 * splat_render — standalone server-side Gaussian-splat rasterizer.
 *
 * The Meta Ray-Ban Display glasses must NOT render splats themselves (tiny
 * browser, no WebGL guarantees, thermal budget), so this process renders
 * frames on the server and the Node server (server.js) streams them to the
 * glasses as JPEGs. It is a persistent child process speaking a trivial
 * line protocol:
 *
 *   stdin  : "P px py pz yaw pitch\n"   render one frame at that camera pose
 *            "Q\n"                       quit
 *   stdout : "F <nbytes>\n" + <nbytes> of JPEG data          per frame
 *   stderr : human-readable log (splat count, per-frame timing)
 *
 * Rendering: classic EWA splatting (Zwicker 2001) the same way the in-repo
 * gl_raster/ compute rasterizer and 3DGS (Kerbl 2023) do it, but on the CPU:
 *   1. project each 3D Gaussian's covariance R·S·Sᵀ·Rᵀ through the
 *      perspective Jacobian to a 2D screen-space ellipse,
 *   2. depth-sort front-to-back,
 *   3. bin ellipses into 32x32 screen tiles (order-preserving),
 *   4. rasterize tiles in parallel (OpenMP), per-pixel transmittance with
 *      early termination — the standard front-to-back "over" operator.
 *
 * SPZ loading reuses the repo's proven decoder (../splat.cpp) unmodified.
 * The world transform matches the desktop renderer: Spark's scale(1,-1,1)
 * — implemented here as a Y-flip on positions and an xy/yz sign-flip on
 * covariances (a mirror can't be folded into the quaternion, but it CAN be
 * folded into the covariance: Σ' = M·Σ·Mᵀ). After that the world is Y-up
 * and not left-right mirrored, exactly like the desktop/Spark view.
 *
 * Usage: splat_render <file.spz> [width height] [jpeg_quality]
 */

#include "../splat.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "third_party/stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace {

struct Prepped {          // per-splat, camera-independent
    float pos[3];         // world (Y-up) position
    float cov[6];         // upper triangle of 3D covariance: xx xy xz yy yz zz
    float rgb[3];         // 0..1
    float alpha;          // 0..1
};

struct Projected {        // per-splat, per-frame
    float sx, sy;         // screen center (pixels)
    float conic[3];       // inverse 2D covariance: a b c  (a x² + 2b xy + c y²)
    float radius;         // 3-sigma extent in pixels
    float depth;          // camera-space z
    float rgb[3], alpha;
};

// Build 3D covariance from quat+scales, then apply the Spark world mirror
// M = diag(1,-1,1):  position y := -y,  cov xy := -xy, cov yz := -yz.
void prep_splats(const std::vector<Splat>& in, std::vector<Prepped>& out) {
    out.resize(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const Splat& s = in[i];
        const double x = s.quat[0], y = s.quat[1], z = s.quat[2], w = s.quat[3];
        // Rotation matrix from quaternion.
        double R[3][3] = {
            {1 - 2*(y*y + z*z),     2*(x*y - w*z),     2*(x*z + w*y)},
            {    2*(x*y + w*z), 1 - 2*(x*x + z*z),     2*(y*z - w*x)},
            {    2*(x*z - w*y),     2*(y*z + w*x), 1 - 2*(x*x + y*y)},
        };
        // Σ = R S Sᵀ Rᵀ, S = diag(scales)  →  Σij = Σk R[i][k] s[k]² R[j][k]
        double s2[3] = { double(s.scales[0]) * s.scales[0],
                         double(s.scales[1]) * s.scales[1],
                         double(s.scales[2]) * s.scales[2] };
        double c[3][3];
        for (int a = 0; a < 3; ++a)
            for (int b = a; b < 3; ++b) {
                double v = 0;
                for (int k = 0; k < 3; ++k) v += R[a][k] * s2[k] * R[b][k];
                c[a][b] = v;
            }
        Prepped& p = out[i];
        p.pos[0] = s.pos[0];
        p.pos[1] = -s.pos[1];          // mirror Y (Spark scale(1,-1,1))
        p.pos[2] = s.pos[2];
        p.cov[0] = float(c[0][0]);
        p.cov[1] = float(-c[0][1]);    // xy flips under diag(1,-1,1)
        p.cov[2] = float(c[0][2]);
        p.cov[3] = float(c[1][1]);
        p.cov[4] = float(-c[1][2]);    // yz flips
        p.cov[5] = float(c[2][2]);
        p.rgb[0] = s.rgba[0] / 255.0f;
        p.rgb[1] = s.rgba[1] / 255.0f;
        p.rgb[2] = s.rgba[2] / 255.0f;
        p.alpha  = s.rgba[3] / 255.0f;
    }
}

struct Camera {
    float px, py, pz;     // world position
    float yaw, pitch;     // radians; yaw about world +Y, pitch up positive
};

class Renderer {
public:
    Renderer(std::vector<Prepped> splats, int w, int h)
        : splats_(std::move(splats)), W(w), H(h) {
        // ~60° vertical FOV — comfortable for a monocular HUD view.
        fy = 0.5f * H / std::tan(0.5f * 60.0f * float(M_PI) / 180.0f);
        fx = fy;
        cx = 0.5f * W;
        cy = 0.5f * H;
        rgb_.resize(std::size_t(W) * H * 3);
        proj_.reserve(splats_.size());
        order_.reserve(splats_.size());
    }

    // Render into the internal RGB8 buffer; returns pointer to it.
    const unsigned char* render(const Camera& cam) {
        // Camera basis (rows of the view rotation). Y-up world; image y
        // grows DOWN, so the second row is the camera "down" vector.
        const float cyaw = std::cos(cam.yaw), syaw = std::sin(cam.yaw);
        const float cpit = std::cos(cam.pitch), spit = std::sin(cam.pitch);
        // forward: yaw=0 looks down -Z (OpenGL-ish), pitch>0 looks up.
        const float fwd[3] = { -syaw * cpit, spit, -cyaw * cpit };
        const float right[3] = { cyaw, 0.0f, -syaw };
        // down = fwd × right  (right-handed: right × fwd = up)
        const float down[3] = {
            fwd[1] * right[2] - fwd[2] * right[1],
            fwd[2] * right[0] - fwd[0] * right[2],
            fwd[0] * right[1] - fwd[1] * right[0],
        };
        const float* Rv[3] = { right, down, fwd };

        proj_.clear();
        // --- 1. project ---------------------------------------------------
        for (const Prepped& p : splats_) {
            const float d[3] = { p.pos[0] - cam.px, p.pos[1] - cam.py,
                                 p.pos[2] - cam.pz };
            const float tz = Rv[2][0]*d[0] + Rv[2][1]*d[1] + Rv[2][2]*d[2];
            if (tz < 0.05f || tz > 200.0f) continue;
            const float tx = Rv[0][0]*d[0] + Rv[0][1]*d[1] + Rv[0][2]*d[2];
            const float ty = Rv[1][0]*d[0] + Rv[1][1]*d[1] + Rv[1][2]*d[2];
            const float inv_z = 1.0f / tz;
            const float sx = fx * tx * inv_z + cx;
            const float sy = fy * ty * inv_z + cy;
            if (sx < -64 || sx > W + 64 || sy < -64 || sy > H + 64) continue;

            // Σcam = Rv Σw Rvᵀ (we need the top-left 2x2 after the Jacobian,
            // so compute the full 3x3 in camera space first).
            float Sw[3][3] = {
                { p.cov[0], p.cov[1], p.cov[2] },
                { p.cov[1], p.cov[3], p.cov[4] },
                { p.cov[2], p.cov[4], p.cov[5] },
            };
            float T[3][3];    // T = Rv · Σw
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b)
                    T[a][b] = Rv[a][0]*Sw[0][b] + Rv[a][1]*Sw[1][b]
                            + Rv[a][2]*Sw[2][b];
            float Sc[3][3];   // Σcam = T · Rvᵀ
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b)
                    Sc[a][b] = T[a][0]*Rv[b][0] + T[a][1]*Rv[b][1]
                             + T[a][2]*Rv[b][2];

            // Perspective Jacobian J = [[fx/z, 0, -fx x/z²],[0, fy/z, -fy y/z²]]
            const float jx = fx * inv_z, jy = fy * inv_z;
            const float jxz = -fx * tx * inv_z * inv_z;
            const float jyz = -fy * ty * inv_z * inv_z;
            // Σ2D = J Σcam Jᵀ  (2x2)
            const float a00 = jx*Sc[0][0] + jxz*Sc[2][0];
            const float a01 = jx*Sc[0][1] + jxz*Sc[2][1];
            const float a02 = jx*Sc[0][2] + jxz*Sc[2][2];
            const float b10 = jy*Sc[1][0] + jyz*Sc[2][0];
            const float b11 = jy*Sc[1][1] + jyz*Sc[2][1];
            const float b12 = jy*Sc[1][2] + jyz*Sc[2][2];
            float v00 = a00*jx + a02*jxz + 0.3f;   // +0.3 px dilation (3DGS)
            float v01 = a01*jy + a02*jyz;
            float v11 = b11*jy + b12*jyz + 0.3f;
            (void)b10;

            const float det = v00 * v11 - v01 * v01;
            if (det <= 1e-8f) continue;
            const float inv_det = 1.0f / det;
            const float mid = 0.5f * (v00 + v11);
            const float lam = mid + std::sqrt(std::max(0.01f, mid*mid - det));
            const float radius = std::ceil(3.0f * std::sqrt(lam));
            if (radius < 0.5f) continue;
            if (sx + radius < 0 || sx - radius > W ||
                sy + radius < 0 || sy - radius > H) continue;

            Projected q;
            q.sx = sx; q.sy = sy;
            q.conic[0] =  v11 * inv_det;
            q.conic[1] = -v01 * inv_det;
            q.conic[2] =  v00 * inv_det;
            q.radius = std::min(radius, 256.0f);
            q.depth = tz;
            q.rgb[0] = p.rgb[0]; q.rgb[1] = p.rgb[1]; q.rgb[2] = p.rgb[2];
            q.alpha = p.alpha;
            proj_.push_back(q);
        }

        // --- 2. global front-to-back sort ---------------------------------
        order_.resize(proj_.size());
        for (std::size_t i = 0; i < order_.size(); ++i) order_[i] = uint32_t(i);
        std::sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) {
            return proj_[a].depth < proj_[b].depth;
        });

        // --- 3. bin into tiles, preserving sorted order --------------------
        const int TX = (W + TILE - 1) / TILE, TY = (H + TILE - 1) / TILE;
        tiles_.assign(std::size_t(TX) * TY, {});
        for (uint32_t oi : order_) {
            const Projected& q = proj_[oi];
            int x0 = std::max(0, int((q.sx - q.radius) / TILE));
            int x1 = std::min(TX - 1, int((q.sx + q.radius) / TILE));
            int y0 = std::max(0, int((q.sy - q.radius) / TILE));
            int y1 = std::min(TY - 1, int((q.sy + q.radius) / TILE));
            for (int ty = y0; ty <= y1; ++ty)
                for (int tx = x0; tx <= x1; ++tx)
                    tiles_[std::size_t(ty) * TX + tx].push_back(oi);
        }

        // --- 4. rasterize tiles in parallel --------------------------------
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 1)
#endif
        for (int ti = 0; ti < TX * TY; ++ti) {
            const int tx0 = (ti % TX) * TILE, ty0 = (ti / TX) * TILE;
            const int tw = std::min(TILE, W - tx0), th = std::min(TILE, H - ty0);
            float acc[TILE * TILE * 3];
            float trans[TILE * TILE];
            std::memset(acc, 0, sizeof(float) * std::size_t(tw) * th * 3);
            for (int k = 0; k < tw * th; ++k) trans[k] = 1.0f;
            int live = tw * th;

            for (uint32_t oi : tiles_[ti]) {
                const Projected& q = proj_[oi];
                const int px0 = std::max(tx0, int(q.sx - q.radius));
                const int px1 = std::min(tx0 + tw - 1, int(q.sx + q.radius));
                const int py0 = std::max(ty0, int(q.sy - q.radius));
                const int py1 = std::min(ty0 + th - 1, int(q.sy + q.radius));
                for (int y = py0; y <= py1; ++y) {
                    const float dy = (y + 0.5f) - q.sy;
                    for (int x = px0; x <= px1; ++x) {
                        const int li = (y - ty0) * tw + (x - tx0);
                        float& T = trans[li];
                        if (T < 0.004f) continue;
                        const float dx = (x + 0.5f) - q.sx;
                        const float power = 0.5f * (q.conic[0]*dx*dx
                                                    + q.conic[2]*dy*dy)
                                          + q.conic[1]*dx*dy;
                        if (power > 4.5f || power < 0.0f) continue;
                        float a = q.alpha * std::exp(-power);
                        if (a < 1.0f/255.0f) continue;
                        if (a > 0.99f) a = 0.99f;
                        const float w = a * T;
                        acc[li*3 + 0] += w * q.rgb[0];
                        acc[li*3 + 1] += w * q.rgb[1];
                        acc[li*3 + 2] += w * q.rgb[2];
                        T *= (1.0f - a);
                        if (T < 0.004f && --live == 0) goto tile_done;
                    }
                }
            }
        tile_done:
            for (int y = 0; y < th; ++y)
                for (int x = 0; x < tw; ++x) {
                    const int li = y * tw + x;
                    unsigned char* dst =
                        &rgb_[(std::size_t(ty0 + y) * W + (tx0 + x)) * 3];
                    for (int c = 0; c < 3; ++c) {
                        float v = acc[li*3 + c];
                        v = v < 0 ? 0 : (v > 1 ? 1 : v);
                        dst[c] = (unsigned char)(v * 255.0f + 0.5f);
                    }
                }
        }
        return rgb_.data();
    }

    int width() const { return W; }
    int height() const { return H; }
    std::size_t last_visible() const { return proj_.size(); }

private:
    static constexpr int TILE = 32;
    std::vector<Prepped> splats_;
    int W, H;
    float fx, fy, cx, cy;
    std::vector<unsigned char> rgb_;
    std::vector<Projected> proj_;
    std::vector<uint32_t> order_;
    std::vector<std::vector<uint32_t>> tiles_;
};

void jpeg_append(void* ctx, void* data, int size) {
    auto* buf = static_cast<std::vector<unsigned char>*>(ctx);
    buf->insert(buf->end(), static_cast<unsigned char*>(data),
                static_cast<unsigned char*>(data) + size);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <file.spz> [width height] [jpeg_quality]\n", argv[0]);
        return 2;
    }
    const int W = argc > 3 ? std::atoi(argv[2]) : 480;
    const int H = argc > 3 ? std::atoi(argv[3]) : 480;
    const int quality = argc > 4 ? std::atoi(argv[4]) : 72;

    std::vector<Splat> raw = splat_load_spz(argv[1]);
    if (raw.empty()) { std::fprintf(stderr, "[render] no splats\n"); return 1; }
    std::vector<Prepped> prepped;
    prep_splats(raw, prepped);
    raw.clear(); raw.shrink_to_fit();

    Renderer r(std::move(prepped), W, H);
    std::fprintf(stderr, "[render] ready: %dx%d q%d\n", W, H, quality);
    std::fflush(stderr);

    std::vector<unsigned char> jpeg;
    char line[256];
    while (std::fgets(line, sizeof(line), stdin)) {
        if (line[0] == 'Q') break;
        if (line[0] != 'P') continue;
        Camera cam{};
        if (std::sscanf(line + 1, "%f %f %f %f %f",
                        &cam.px, &cam.py, &cam.pz, &cam.yaw, &cam.pitch) != 5)
            continue;
        const auto t0 = std::chrono::steady_clock::now();
        const unsigned char* rgb = r.render(cam);
        const auto t1 = std::chrono::steady_clock::now();
        jpeg.clear();
        stbi_write_jpg_to_func(jpeg_append, &jpeg, W, H, 3, rgb, quality);
        const auto t2 = std::chrono::steady_clock::now();
        std::printf("F %zu\n", jpeg.size());
        std::fwrite(jpeg.data(), 1, jpeg.size(), stdout);
        std::fflush(stdout);
        std::fprintf(stderr,
            "[render] %zu visible, render %.1fms jpeg %.1fms (%zu bytes)\n",
            r.last_visible(),
            std::chrono::duration<double, std::milli>(t1 - t0).count(),
            std::chrono::duration<double, std::milli>(t2 - t1).count(),
            jpeg.size());
        std::fflush(stderr);
    }
    return 0;
}
