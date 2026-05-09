#pragma once

#include <cstdint>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#else
#include <epoxy/gl.h>
#endif

#include "splat.h"

// GPU-side storage for a Spark-compatible Gaussian-splat scene.
//
//   * The splat data is packed into TWO RGBA32UI 2D textures (8 bytes
//     per channel × 4 channels = 32 bytes per splat per texture, but
//     each splat takes ONE texel of texture A AND ONE texel of texture
//     B — net 32 bytes total). The packing matches Spark's
//     packSplatExt:
//
//       texelA.x = packHalf2x16(pos.xy)
//       texelA.y = packHalf2x16(vec2(pos.z, quat.x))
//       texelA.z = packHalf2x16(vec2(quat.y, quat.z))
//       texelA.w = packHalf2x16(vec2(alpha, 0))
//       texelB.x = packHalf2x16(rgb.rg)
//       texelB.y = packHalf2x16(vec2(rgb.b, log(scale.x)))
//       texelB.z = packHalf2x16(vec2(log(scale.y), log(scale.z)))
//       texelB.w = packHalf2x16(vec2(quat.w, 0))
//
//   * A separate R32UI 2D "ordering" texture holds the per-frame
//     back-to-front sort indices. Re-uploaded each frame the camera
//     direction changes — only ~4 bytes per splat, so even a
//     ~2 M-splat scene takes <8 MB to refresh.
//
// The vertex shader reads ordering → splat-index → splat-data via
// indirect texelFetch chain, exactly as Spark does in splatVertex.glsl.

struct PackedSplats {
    GLuint texA       = 0;        // RGBA32UI, half-packed splat half 1
    GLuint texB       = 0;        // RGBA32UI, half-packed splat half 2
    GLuint texOrder   = 0;        // R32UI, sort indices
    int    count      = 0;        // number of splats
    int    tex_width  = 0;        // texture width (both A/B/order use this)
    int    tex_height = 0;
};

// Build texA/texB from a freshly-loaded Splat array. Allocates GPU
// textures, uploads pixel data, and primes the ordering texture with
// identity indices [0..N). After this, only the ordering texture
// gets updated per frame.
void packed_splats_upload(PackedSplats& ps, const std::vector<Splat>& splats);

// Sort splats back-to-front from the WORLD-space camera POSITION
// (radial sort) and upload the new index list to the ordering
// texture. Spark's `sortRadial = true` default — at wide FOV the
// z-projection sort puts edge-of-frustum splats in slightly wrong
// slots, which reads as "splats look big and shifty as I rotate"
// near the viewport edges. Radial sort fixes that because each
// splat's distance from the camera is rotation-invariant.
//
// `cam_dir` is still used for the early-out skip when the camera
// has barely moved, matching Spark's per-frame `viewChanged`
// heuristic. `model` is the splat-cloud model matrix.
void packed_splats_sort_and_upload(PackedSplats& ps,
                                   const float cam_pos[3],
                                   const float cam_dir[3],
                                   const float model[16],
                                   const std::vector<Splat>& source,
                                   float cos_threshold = 0.999f);

void packed_splats_free(PackedSplats& ps);

// Half-float helpers shared with the dump path so we can simulate
// Spark's PackedSplats pack→unpack roundtrip on the CPU.
uint16_t to_half(float f);
float    from_half(uint16_t h);

// Spark's PackedSplats per-field quantisation. Apply each before
// uploading to the GPU so the splat shader sees the SAME quantised
// values Spark's WebGL shader does (scales bucketed to 8-bit log
// levels, quats bucketed to oct-XY 8+8 + 8-bit angle, etc.). Without
// this the native shader processes finer half-float data than Spark
// — the rendering reads as more granular even though the source SPZ
// is the same.
float roundtrip_half_quant(float f);
float roundtrip_scale_quant(float scale);
void  roundtrip_quat_quant(double& x, double& y, double& z, double& w);
