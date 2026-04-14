#pragma once

// ===========================================================================
// Cloth-simulated flag (verlet integration).
// ===========================================================================
//
// Pure C++ module — no GL, no platform headers. The flag's particles live
// in NDC space (-1..+1 on each axis) so the render path can hand them
// straight to the flat-shaded highlight shader without a per-frame
// pixel-to-NDC conversion. Because NDC is anisotropic on non-square
// windows, flag_init takes width/height in pixels and pre-scales the
// constraint rest lengths so the cloth keeps a consistent on-screen
// aspect ratio across portrait/landscape/ultra-wide windows.
//
// The cloth is a COLS×ROWS grid of particles. Structural constraints
// connect horizontal and vertical neighbours; there are no shear or
// bending constraints (the wind does all the visible work and shear
// springs would just damp out the ripple). The leftmost column is
// pinned to the top of the stick.
//
// All per-frame force + constraint work happens in flag_update; the
// render path calls flag_build_triangles to turn the deformed grid into
// a triangle list.

#include <vector>

struct ClothParticle {
    // (x, y) are NDC render positions. z is an extra "depth" dimension
    // that never gets drawn — it only exists so the cross-product
    // normal computed in flag_build_triangles reflects out-of-plane
    // bulging under wind. Without z the cloth would be perfectly flat
    // and the Lambert shading would be uniform.
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    bool  pinned = false;
};

struct ClothFlag {
    static constexpr int COLS = 14;
    static constexpr int ROWS = 9;
    static constexpr int N    = COLS * ROWS;

    std::vector<ClothParticle> p;  // N particles, row-major: p[row*COLS + col]

    // Rest lengths for horizontal and vertical structural constraints,
    // stored in NDC units. Recomputed by flag_init when the window
    // size changes.
    float rest_h = 0.0f;
    float rest_v = 0.0f;

    // Anchor of the top-left (pinned) particle, in NDC. This is where
    // the flag meets the top of the stick.
    float anchor_x = 0.0f;
    float anchor_y = 0.0f;

    // Window size the flag was last initialised for; the render path
    // re-inits when these stop matching the current drawable size.
    int   inited_w = 0;
    int   inited_h = 0;
};

// (Re)initialise the flag to a flat rectangle anchored to the top of
// the stick. width/height are the current drawable size in pixels;
// they're used both to pick a screen-proportional cloth size and to
// map pixel-space rest lengths back to NDC so the cloth isn't
// squished on anisotropic windows.
void flag_init(ClothFlag& f, int width, int height);

// One verlet step. dt is seconds since the last call (clamp to
// <= 0.02s upstream to keep the integrator stable). time_s is an
// absolute time in seconds, fed to the wind function so the flag
// keeps rippling across consecutive frames.
void flag_update(ClothFlag& f, float dt, float time_s);

// Triangulate the current deformed grid. Writes (COLS-1)*(ROWS-1)*2
// triangles = (COLS-1)*(ROWS-1)*6 vertices into `out`. Each vertex
// is 5 floats: x, y (NDC render position) followed by r, g, b
// (per-vertex color from normal-based half-Lambert lighting with
// front/back tinting — inspired by shadertoy MldXWX's findnormal).
void flag_build_triangles(const ClothFlag& f, std::vector<float>& out);

// Axis-aligned bounding box of the deformed cloth in NDC. Used by
// the hit-test so the click area tracks the rippling cloth.
void flag_bbox(const ClothFlag& f,
               float& out_x0, float& out_y0,
               float& out_x1, float& out_y1);
