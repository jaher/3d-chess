#include "cloth_flag.h"

#include <algorithm>
#include <cmath>

// ===========================================================================
// Cloth flag — verlet integration with distance constraints.
// ===========================================================================
//
// Tuning values below were all picked by eye. The grid is small enough
// (14*9 = 126 particles, ~223 constraints, 4 iters per tick) that the
// whole simulation costs ~10k float ops per frame on both desktop and
// web. There is no perf budget to worry about; if the cloth looks off,
// change the constants instead of the algorithm.

// Flag anchor sits near the bottom-right corner of the window. The
// cloth extends to the right of this anchor (pinned column = col 0),
// so picking values close to (1, -1) puts the whole stick+flag in
// the corner. Leave a small margin for the ripple to not clip.
static constexpr float STICK_X  =  0.88f;
static constexpr float ANCHOR_Y = -0.82f;

// Target on-screen flag size, expressed as a fraction of the shorter
// window dimension.
static constexpr float FLAG_H_FRACTION = 0.0225f;
static constexpr float FLAG_W_FRACTION = 0.0325f;

// Constraint solver iterations per tick. Higher = stiffer cloth.
static constexpr int   CONSTRAINT_ITERS = 6;

// Verlet damping coefficient applied to (pos - prev_pos) each step.
// Values very close to 1 let the cloth flap freely; lower values
// dissipate motion quickly. 0.985 gives a nicely lively cloth that
// still settles when the wind drops.
static constexpr float DAMPING = 0.985f;

// Gravity (NDC units per second squared). Our "mass" scale is
// arbitrary, so this isn't 9.8 — it's whatever makes the cloth look
// like it has weight without snapping taut.
static constexpr float GRAVITY_Y = -0.35f;

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------
void flag_init(ClothFlag& f, int width, int height) {
    if (width <= 0)  width  = 1;
    if (height <= 0) height = 1;

    // Desired cloth size in pixels, scaled off the shorter window edge
    // so the flag keeps its visual weight regardless of aspect ratio.
    const float short_px = static_cast<float>(std::min(width, height));
    const float flag_w_px = short_px * FLAG_W_FRACTION;
    const float flag_h_px = short_px * FLAG_H_FRACTION;

    const float cell_w_px = flag_w_px / static_cast<float>(ClothFlag::COLS - 1);
    const float cell_h_px = flag_h_px / static_cast<float>(ClothFlag::ROWS - 1);

    // Map pixel deltas back to NDC deltas. NDC spans -1..+1 (width 2)
    // over `width` pixels, so one pixel = 2/width NDC units.
    f.rest_h = cell_w_px * 2.0f / static_cast<float>(width);
    f.rest_v = cell_h_px * 2.0f / static_cast<float>(height);

    f.anchor_x = STICK_X;
    f.anchor_y = ANCHOR_Y;
    f.inited_w = width;
    f.inited_h = height;

    f.p.assign(ClothFlag::N, ClothParticle{});
    for (int row = 0; row < ClothFlag::ROWS; ++row) {
        for (int col = 0; col < ClothFlag::COLS; ++col) {
            ClothParticle& q = f.p[row * ClothFlag::COLS + col];
            q.x  = f.anchor_x + f.rest_h * static_cast<float>(col);
            q.y  = f.anchor_y - f.rest_v * static_cast<float>(row);
            q.z  = 0.0f;
            q.px = q.x;
            q.py = q.y;
            q.pz = 0.0f;
            q.pinned = (col == 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Per-frame update
// ---------------------------------------------------------------------------
// Time-scale applied to every sinusoid below. Lower values = calmer,
// more slowly swaying cloth.
static constexpr float WIND_TIME_SCALE = 0.55f;

// Wind force functions. Spatial frequencies are expressed per-cell
// (cf and rf are the float column/row index) so a coefficient of ~1
// means one full wave per cell. With COLS=14 and ROWS=9, coefficients
// around 1.0–1.3 give two to three visible ripples across the cloth —
// the thing that actually makes it look wavy. Previously these were
// keyed off NDC world coords, which on our ~0.04 NDC-tall cloth meant
// the phase barely moved and the flag just pulsed as a block.
//
// Inspired by the cloth-texture motion in shadertoy MldXWX: the
// shader itself reads a pre-simulated position texture, but the
// characteristic look comes from superposed high-frequency waves
// in the underlying sim. We get the same effect in our small grid
// by cranking the per-cell spatial frequency.
static inline float wind_x(float cf, float rf, float t) {
    const float ts = t * WIND_TIME_SCALE;
    return 2.0f + 0.6f * std::sin(ts * 1.3f + rf * 1.1f)
                + 0.3f * std::sin(ts * 2.7f + cf * 0.8f);
}

static inline float wind_y(float cf, float rf, float t) {
    const float ts = t * WIND_TIME_SCALE;
    return 0.20f * std::sin(ts * 2.1f + cf * 0.9f + rf * 0.6f);
}

static inline float wind_z(float cf, float rf, float t) {
    // Three superposed travelling waves along the length of the flag
    // (cf grows from 0 at the stick to COLS-1 at the free edge). High
    // per-col frequencies (1.5, 2.2, 1.1) give roughly 3, 5, and 2.5
    // full wavelengths across our 14-wide flag, which reads as a
    // clearly rippling cloth rather than a single bulge.
    const float ts = t * WIND_TIME_SCALE;
    return 3.6f * std::sin(ts * 2.4f + cf * 1.50f + rf * 0.6f)
         + 2.4f * std::sin(ts * 3.1f + cf * 2.20f - rf * 0.3f)
         + 1.8f * std::sin(ts * 1.6f + cf * 1.10f);
}

void flag_update(ClothFlag& f, float dt, float time_s) {
    if (f.p.empty()) return;
    if (dt <= 0.0f) return;

    const float dt2 = dt * dt;

    // 1. Integrate forces into new positions.
    for (int row = 0; row < ClothFlag::ROWS; ++row) {
        for (int col = 0; col < ClothFlag::COLS; ++col) {
            ClothParticle& q = f.p[row * ClothFlag::COLS + col];
            if (q.pinned) {
                // Keep the pinned column glued to the initial anchor
                // positions. No integration.
                q.x  = f.anchor_x;
                q.y  = f.anchor_y - f.rest_v * static_cast<float>(row);
                q.z  = 0.0f;
                q.px = q.x;
                q.py = q.y;
                q.pz = 0.0f;
                continue;
            }

            // Wind force: scaled down further from the stick so the
            // cloth unfurls smoothly rather than snapping outwards at
            // the trailing edge. The (col+1) / COLS ramp also prevents
            // the pinned column's neighbour from being yanked too hard.
            // Wind functions are parameterised by grid indices, not
            // NDC position, so spatial frequencies are expressed per
            // cell and ripples are visible on our small flag.
            const float cf    = static_cast<float>(col);
            const float rf    = static_cast<float>(row);
            const float reach = cf / static_cast<float>(ClothFlag::COLS - 1);
            const float ax = wind_x(cf, rf, time_s) * (0.35f + 0.65f * reach);
            const float ay = GRAVITY_Y + wind_y(cf, rf, time_s);
            const float az = wind_z(cf, rf, time_s) * (0.20f + 0.80f * reach);

            // Verlet step with damping, all three axes.
            const float vx = (q.x - q.px) * DAMPING;
            const float vy = (q.y - q.py) * DAMPING;
            const float vz = (q.z - q.pz) * DAMPING;
            const float nx = q.x + vx + ax * dt2;
            const float ny = q.y + vy + ay * dt2;
            const float nz = q.z + vz + az * dt2;
            q.px = q.x;
            q.py = q.y;
            q.pz = q.z;
            q.x  = nx;
            q.y  = ny;
            q.z  = nz;
        }
    }

    // 2. Relax structural constraints iteratively. Distances are
    //    computed in 3D (x, y, z) so the cloth keeps its surface
    //    area as it bulges out-of-plane under wind.
    for (int iter = 0; iter < CONSTRAINT_ITERS; ++iter) {
        // Horizontal springs (connecting col and col+1 in the same row)
        for (int row = 0; row < ClothFlag::ROWS; ++row) {
            for (int col = 0; col < ClothFlag::COLS - 1; ++col) {
                ClothParticle& a = f.p[row * ClothFlag::COLS + col];
                ClothParticle& b = f.p[row * ClothFlag::COLS + col + 1];
                float dx = b.x - a.x;
                float dy = b.y - a.y;
                float dz = b.z - a.z;
                float d  = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d < 1e-6f) continue;
                float diff = (d - f.rest_h) / d;
                float mx = dx * 0.5f * diff;
                float my = dy * 0.5f * diff;
                float mz = dz * 0.5f * diff;
                if (a.pinned && !b.pinned) {
                    b.x -= dx * diff;
                    b.y -= dy * diff;
                    b.z -= dz * diff;
                } else if (!a.pinned && b.pinned) {
                    a.x += dx * diff;
                    a.y += dy * diff;
                    a.z += dz * diff;
                } else if (!a.pinned && !b.pinned) {
                    a.x += mx;  a.y += my;  a.z += mz;
                    b.x -= mx;  b.y -= my;  b.z -= mz;
                }
            }
        }
        // Vertical springs (connecting row and row+1 in the same column)
        for (int col = 0; col < ClothFlag::COLS; ++col) {
            for (int row = 0; row < ClothFlag::ROWS - 1; ++row) {
                ClothParticle& a = f.p[row * ClothFlag::COLS + col];
                ClothParticle& b = f.p[(row + 1) * ClothFlag::COLS + col];
                float dx = b.x - a.x;
                float dy = b.y - a.y;
                float dz = b.z - a.z;
                float d  = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d < 1e-6f) continue;
                float diff = (d - f.rest_v) / d;
                float mx = dx * 0.5f * diff;
                float my = dy * 0.5f * diff;
                float mz = dz * 0.5f * diff;
                if (a.pinned && !b.pinned) {
                    b.x -= dx * diff;
                    b.y -= dy * diff;
                    b.z -= dz * diff;
                } else if (!a.pinned && b.pinned) {
                    a.x += dx * diff;
                    a.y += dy * diff;
                    a.z += dz * diff;
                } else if (!a.pinned && !b.pinned) {
                    a.x += mx;  a.y += my;  a.z += mz;
                    b.x -= mx;  b.y -= my;  b.z -= mz;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Rendering helpers
// ---------------------------------------------------------------------------

// Look up particle (col, row) in 3D, clamped to the valid range. Used
// by the normal computation at grid edges — this reproduces the
// one-sided difference that the shadertoy's findnormal glosses over
// with its textureLod sampling.
static inline void get_xyz(const ClothFlag& f, int col, int row,
                           float& x, float& y, float& z) {
    if (col < 0) col = 0;
    if (col > ClothFlag::COLS - 1) col = ClothFlag::COLS - 1;
    if (row < 0) row = 0;
    if (row > ClothFlag::ROWS - 1) row = ClothFlag::ROWS - 1;
    const ClothParticle& q = f.p[row * ClothFlag::COLS + col];
    x = q.x;
    y = q.y;
    z = q.z;
}

// Per-vertex color from a cross-product normal, in the style of
// shadertoy MldXWX's findnormal + lighting. Our cloth is 2D on-screen
// but has a fake z dimension under the wind, so the normal we compute
// here varies meaningfully across the surface.
//
// Writes r, g, b into out. The color already includes front/back
// tinting and a half-Lambert shade term.
static void shade_vertex(const ClothFlag& f, int col, int row,
                         float& out_r, float& out_g, float& out_b) {
    float xL, yL, zL;  get_xyz(f, col - 1, row, xL, yL, zL);
    float xR, yR, zR;  get_xyz(f, col + 1, row, xR, yR, zR);
    float xD, yD, zD;  get_xyz(f, col, row - 1, xD, yD, zD);
    float xU, yU, zU;  get_xyz(f, col, row + 1, xU, yU, zU);

    // Tangent vectors (horizontal and vertical). The shadertoy's
    // version uses textureLod deltas; we use discrete neighbour
    // deltas and it amounts to the same thing.
    const float tx_x = xR - xL;
    const float tx_y = yR - yL;
    const float tx_z = zR - zL;
    const float ty_x = xU - xD;
    const float ty_y = yU - yD;
    const float ty_z = zU - zD;

    // n = cross(ty, tx) so n.z comes out positive when the cloth is
    // facing the camera (+z viewer). Standard right-handed cross.
    float nx = ty_y * tx_z - ty_z * tx_y;
    float ny = ty_z * tx_x - ty_x * tx_z;
    float nz = ty_x * tx_y - ty_y * tx_x;
    float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (nlen < 1e-8f) {
        // Degenerate triangle — fall back to "facing camera".
        nx = 0.0f; ny = 0.0f; nz = 1.0f;
    } else {
        nx /= nlen; ny /= nlen; nz /= nlen;
    }

    // Front/back detection: if the normal points toward the viewer
    // (+z) we see the front of the cloth; if it points away we see
    // the back. The shadertoy flips the normal and re-tints in that
    // case; we match that.
    bool back = (nz < 0.0f);
    if (back) {
        nx = -nx; ny = -ny; nz = -nz;
    }

    // Half-Lambert lighting — same as the shadertoy's `lighting`:
    // dot(N, L) * 0.5 + 0.5. Light points up-and-in from the upper
    // left, giving the rippled face of the flag a natural gradient.
    const float lx = -0.45f, ly = 0.55f, lz = 0.70f;
    const float ll = std::sqrt(lx * lx + ly * ly + lz * lz);
    const float ldot = (nx * lx + ny * ly + nz * lz) / ll;
    float shade = ldot * 0.5f + 0.5f;
    if (shade < 0.25f) shade = 0.25f;   // never go pitch black
    if (shade > 1.10f) shade = 1.10f;

    // Front / back base tints. Front is a warm off-white, back is a
    // cooler greyer white (the shadertoy's blue/peach tint palette
    // translated to a white flag rather than a orange curtain).
    float base_r, base_g, base_b;
    if (back) {
        base_r = 0.82f;
        base_g = 0.85f;
        base_b = 0.92f;
    } else {
        base_r = 1.00f;
        base_g = 0.98f;
        base_b = 0.94f;
    }

    out_r = base_r * shade;
    out_g = base_g * shade;
    out_b = base_b * shade;
    if (out_r > 1.0f) out_r = 1.0f;
    if (out_g > 1.0f) out_g = 1.0f;
    if (out_b > 1.0f) out_b = 1.0f;
}

void flag_build_triangles(const ClothFlag& f, std::vector<float>& out) {
    out.clear();
    if (f.p.empty()) return;
    const int quad_count = (ClothFlag::COLS - 1) * (ClothFlag::ROWS - 1);
    // 6 verts per quad, 5 floats per vert (x, y, r, g, b).
    out.reserve(static_cast<size_t>(quad_count) * 6 * 5);

    // Precompute shaded colors at every grid vertex.
    std::vector<float> colr(ClothFlag::N), colg(ClothFlag::N), colb(ClothFlag::N);
    for (int row = 0; row < ClothFlag::ROWS; ++row) {
        for (int col = 0; col < ClothFlag::COLS; ++col) {
            float r, g, b;
            shade_vertex(f, col, row, r, g, b);
            int idx = row * ClothFlag::COLS + col;
            colr[idx] = r;
            colg[idx] = g;
            colb[idx] = b;
        }
    }

    auto emit = [&](int col, int row) {
        const ClothParticle& q = f.p[row * ClothFlag::COLS + col];
        int idx = row * ClothFlag::COLS + col;
        out.push_back(q.x);
        out.push_back(q.y);
        out.push_back(colr[idx]);
        out.push_back(colg[idx]);
        out.push_back(colb[idx]);
    };

    for (int row = 0; row < ClothFlag::ROWS - 1; ++row) {
        for (int col = 0; col < ClothFlag::COLS - 1; ++col) {
            // Two tris: (a, b, c) and (b, d, c)
            emit(col,     row);
            emit(col + 1, row);
            emit(col,     row + 1);
            emit(col + 1, row);
            emit(col + 1, row + 1);
            emit(col,     row + 1);
        }
    }
}

void flag_bbox(const ClothFlag& f,
               float& out_x0, float& out_y0,
               float& out_x1, float& out_y1) {
    if (f.p.empty()) {
        out_x0 = out_y0 = out_x1 = out_y1 = 0.0f;
        return;
    }
    out_x0 = out_x1 = f.p[0].x;
    out_y0 = out_y1 = f.p[0].y;
    for (const auto& q : f.p) {
        if (q.x < out_x0) out_x0 = q.x;
        if (q.x > out_x1) out_x1 = q.x;
        if (q.y < out_y0) out_y0 = q.y;
        if (q.y > out_y1) out_y1 = q.y;
    }
}
