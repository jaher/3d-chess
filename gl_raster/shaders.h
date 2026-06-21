#pragma once

// GLSL compute shader sources for the tile-based gsplat rasterizer.
//
// One-for-one port of native/cuda_raster/kernels.cu using GL 4.3+
// compute shaders. The bindings below stay stable across shaders —
// every shader sees the same SSBO layout:
//
//   binding 0 : SplatGPU[]           in  (the raw splat data)
//   binding 1 : PreprocessedSplat[]  out (per-splat projected state)
//   binding 2 : uint[]               out (touched-tile count per splat
//                                         — preprocess only; later
//                                         reused as the per-tile
//                                         splat-id array)
//   binding 3 : uint64[]             out (sort keys: (tile<<32)|depth)
//   binding 4 : uint[]               out (sort values: splat indices)
//   binding 5 : uint[2]              out (tile ranges: (start,end))
//   binding 6 : uint                 out (atomic counter for emit)
//
// The output image is `image2D` binding 0, GL_RGBA8.

namespace gl_raster {

// Shared GLSL chunk — struct definitions + helper functions that
// every compute shader needs. The two structs match the CUDA
// `SplatGPU` and `PreprocessedSplat` byte-layouts so SSBOs are
// interchangeable between the two backends.
inline constexpr const char* kGlslCommon = R"GLSL(
#version 430
#extension GL_ARB_gpu_shader_int64 : enable

// std430 layout — alignment matches CUDA's natural alignment exactly
// (vec3 padded to vec4, etc.).
struct SplatGPU {
    vec3  pos;        float _pad0;
    vec3  scales;     float _pad1;
    vec4  quat;       // (x,y,z,w)
    vec4  rgba;       // [0..1]
};

struct PreprocessedSplat {
    vec2  xy;
    float depth;
    float radius;
    vec3  conic;      // inverse-cov2D (a, b, c)
    float alpha;
    vec3  color;
    float _pad;
    uvec2 tile_min;   // (tile_x_min, tile_y_min)
    uvec2 tile_max;
};

// quatVec / quatQuat / scaleQuaternionToMatrix — same as the OpenGL
// hand-port + the dyno-emitted splat shaders.
vec3 quat_vec(vec4 q, vec3 v) {
    vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}
vec4 quat_quat(vec4 q1, vec4 q2) {
    return vec4(
        q1.w*q2.x + q1.x*q2.w + q1.y*q2.z - q1.z*q2.y,
        q1.w*q2.y - q1.x*q2.z + q1.y*q2.w + q1.z*q2.x,
        q1.w*q2.z + q1.x*q2.y - q1.y*q2.x + q1.z*q2.w,
        q1.w*q2.w - q1.x*q2.x - q1.y*q2.y - q1.z*q2.z);
}

// 16x16 tiles — must stay in sync with the CUDA constants.
const uint TILE_SIZE_X = 16u;
const uint TILE_SIZE_Y = 16u;
const float MAX_ALPHA = 0.99;
const float MIN_T     = 1e-4;
)GLSL";

// ---------------------------------------------------------------------
// Preprocess — one invocation per splat. Mirrors
// cuda_raster/kernels.cu preprocess_kernel verbatim.
// ---------------------------------------------------------------------
inline constexpr const char* kGlslPreprocess = R"GLSL(
layout(local_size_x = 256) in;

layout(std430, binding = 0) readonly  buffer Splats     { SplatGPU  splats[]; };
layout(std430, binding = 1) writeonly buffer Pre        { PreprocessedSplat pre[]; };
layout(std430, binding = 2) writeonly buffer Touched    { uint touched[]; };

layout(location = 0)  uniform int   N;
layout(location = 1)  uniform float mesh_scale;
layout(location = 2)  uniform vec4  mesh_quat;
layout(location = 3)  uniform vec3  mesh_translate;
layout(location = 4)  uniform vec4  view_quat;
layout(location = 5)  uniform vec3  view_pos;
layout(location = 6)  uniform mat4  proj;
layout(location = 7)  uniform int   render_w;
layout(location = 8)  uniform int   render_h;
layout(location = 9)  uniform int   tile_grid_x;
layout(location = 10) uniform int   tile_grid_y;
layout(location = 11) uniform float max_pixel_radius;
layout(location = 12) uniform float blur_amount;

void main() {
    int i = int(gl_GlobalInvocationID.x);
    if (i >= N) return;

    SplatGPU s = splats[i];
    vec3 center = s.pos * mesh_scale;
    center = quat_vec(mesh_quat, center) + mesh_translate;
    vec3 scales = s.scales * mesh_scale;
    vec4 q = quat_quat(mesh_quat, s.quat);

    // High-α boost (rgba.a *= 2.0; saturating curve).
    float alpha = s.rgba.a * 2.0;
    if (alpha > 1.0) {
        alpha = min(alpha * 4.0 - 3.0, 5.0);
    }

    vec3 view_center = quat_vec(view_quat, center) + view_pos;
    if (view_center.z >= -0.2) { touched[i] = 0u; return; }

    vec4 clip = proj * vec4(view_center, 1.0);
    if (abs(clip.z) >= clip.w) { touched[i] = 0u; return; }
    float clip_xy = 1.4 * clip.w;
    if (abs(clip.x) > clip_xy || abs(clip.y) > clip_xy) {
        touched[i] = 0u; return;
    }

    float ndc_x = clip.x / clip.w;
    float ndc_y = clip.y / clip.w;
    float px = (ndc_x * 0.5 + 0.5) * float(render_w);
    float py = (ndc_y * 0.5 + 0.5) * float(render_h);

    // View-space 3x3 cov: cov3D = RS·RSᵀ
    vec4 vq = quat_quat(view_quat, q);
    float r00 = 1.0 - 2.0 * (vq.y*vq.y + vq.z*vq.z);
    float r10 =        2.0 * (vq.x*vq.y + vq.w*vq.z);
    float r20 =        2.0 * (vq.x*vq.z - vq.w*vq.y);
    float r01 =        2.0 * (vq.x*vq.y - vq.w*vq.z);
    float r11 = 1.0 - 2.0 * (vq.x*vq.x + vq.z*vq.z);
    float r21 =        2.0 * (vq.y*vq.z + vq.w*vq.x);
    float r02 =        2.0 * (vq.x*vq.z + vq.w*vq.y);
    float r12 =        2.0 * (vq.y*vq.z - vq.w*vq.x);
    float r22 = 1.0 - 2.0 * (vq.x*vq.x + vq.y*vq.y);

    float RS00 = r00*scales.x, RS01 = r01*scales.y, RS02 = r02*scales.z;
    float RS10 = r10*scales.x, RS11 = r11*scales.y, RS12 = r12*scales.z;
    float RS20 = r20*scales.x, RS21 = r21*scales.y, RS22 = r22*scales.z;

    float c00 = RS00*RS00 + RS01*RS01 + RS02*RS02;
    float c01 = RS00*RS10 + RS01*RS11 + RS02*RS12;
    float c02 = RS00*RS20 + RS01*RS21 + RS02*RS22;
    float c11 = RS10*RS10 + RS11*RS11 + RS12*RS12;
    float c12 = RS10*RS20 + RS11*RS21 + RS12*RS22;
    float c22 = RS20*RS20 + RS21*RS21 + RS22*RS22;

    float fx = 0.5 * float(render_w) * proj[0][0];
    float fy = 0.5 * float(render_h) * proj[1][1];
    float invZ = 1.0 / view_center.z;
    float J00 = fx * invZ;
    float J02 = -fx * view_center.x * invZ * invZ;
    float J11 = fy * invZ;
    float J12 = -fy * view_center.y * invZ * invZ;

    float T00 = J00*c00 + J02*c02;
    float T01 = J00*c01 + J02*c12;
    float T02 = J00*c02 + J02*c22;
    float T10 = J11*c01 + J12*c02;
    float T11 = J11*c11 + J12*c12;
    float T12 = J11*c12 + J12*c22;

    // EWA 2D covariance, then Mip-Splatting low-pass dilation: adding
    // blur_amount to the diagonal keeps sub-pixel splats from aliasing.
    // Keep the PRE-dilation determinant so we can renormalize opacity
    // below — a dilated gaussian must fade, not brighten.
    float a_orig = T00*J00 + T02*J02;
    float b      = T01*J11 + T02*J12;
    float d_orig = T11*J11 + T12*J12;
    float det_orig = a_orig * d_orig - b * b;
    float a = a_orig + blur_amount;
    float d = d_orig + blur_amount;

    float det = a * d - b * b;
    if (det <= 0.0) { touched[i] = 0u; return; }
    float inv_det = 1.0 / det;
    vec3 conic = vec3(d*inv_det, -b*inv_det, a*inv_det);

    // Mip-Splatting opacity compensation (ported from the viewer's
    // gl_raster): the low-pass dilation grew the splat's footprint, so
    // scale alpha by √(det_orig/det) to conserve total energy. Without
    // it, small/distant splats render too bright and soft — the drift
    // that made the chess backdrop read less crisp than the viewer.
    alpha *= sqrt(max(0.0, det_orig / det));

    float mid = 0.5 * (a + d);
    float disc = max(0.0, mid*mid - det);
    float eig_max = mid + sqrt(disc);
    float radius = ceil(3.0 * sqrt(eig_max));
    if (radius > max_pixel_radius) radius = max_pixel_radius;
    if (radius < 1.0) { touched[i] = 0u; return; }

    int min_tx = max(0,
        int(floor((px - radius) / float(TILE_SIZE_X))));
    int max_tx = min(tile_grid_x - 1,
        int(floor((px + radius) / float(TILE_SIZE_X))));
    int min_ty = max(0,
        int(floor((py - radius) / float(TILE_SIZE_Y))));
    int max_ty = min(tile_grid_y - 1,
        int(floor((py + radius) / float(TILE_SIZE_Y))));
    if (max_tx < min_tx || max_ty < min_ty) {
        touched[i] = 0u; return;
    }

    PreprocessedSplat ps;
    ps.xy       = vec2(px, py);
    ps.depth    = -view_center.z;
    ps.radius   = radius;
    ps.conic    = conic;
    ps.alpha    = min(alpha, 1.0);
    ps.color    = s.rgba.rgb;
    ps.tile_min = uvec2(min_tx, min_ty);
    ps.tile_max = uvec2(max_tx, max_ty);
    pre[i] = ps;

    touched[i] = uint((max_tx - min_tx + 1) * (max_ty - min_ty + 1));
}
)GLSL";

// ---------------------------------------------------------------------
// Duplicate — every visible splat emits one (key, value) tuple per
// touched tile, using a single global atomic counter to find its
// slot. No prefix sum needed; the radix sort takes care of order.
// ---------------------------------------------------------------------
inline constexpr const char* kGlslDuplicate = R"GLSL(
layout(local_size_x = 256) in;

layout(std430, binding = 1) readonly buffer Pre      { PreprocessedSplat pre[]; };
layout(std430, binding = 2) readonly buffer Touched  { uint touched[]; };
layout(std430, binding = 3) writeonly buffer Keys    { uint64_t keys[]; };
layout(std430, binding = 4) writeonly buffer Values  { uint values[]; };
layout(std430, binding = 6) buffer Counter           { uint emit_counter; };

layout(location = 0) uniform int  N;
layout(location = 1) uniform int  tile_grid_x;

void main() {
    int i = int(gl_GlobalInvocationID.x);
    if (i >= N) return;
    uint count = touched[i];
    if (count == 0u) return;
    PreprocessedSplat ps = pre[i];
    // Reserve `count` slots in one atomic.
    uint base = atomicAdd(emit_counter, count);
    uint depth_bits = floatBitsToUint(ps.depth);
    uint off = base;
    for (uint ty = ps.tile_min.y; ty <= ps.tile_max.y; ++ty) {
        for (uint tx = ps.tile_min.x; tx <= ps.tile_max.x; ++tx) {
            uint tile_id = ty * uint(tile_grid_x) + tx;
            keys[off]   = (uint64_t(tile_id) << 32)
                        | uint64_t(depth_bits);
            values[off] = uint(i);
            ++off;
        }
    }
}
)GLSL";

// ---------------------------------------------------------------------
// Radix sort — 8-bit-per-pass on uint64 keys + uint values.
//
// Each pass is three dispatches:
//   1. count_kernel    — per workgroup, count how many keys fall in
//                        each of 256 buckets for the current digit;
//                        write local-bucket-counts to the global
//                        per-group histogram.
//   2. scan_kernel     — exclusive scan over the (groups × 256)
//                        histogram, producing the global write
//                        offset for each (group, bucket) pair.
//   3. scatter_kernel  — re-read each key, look up its (group,bucket)
//                        offset, atomic-add to it, write the key/value
//                        to its sorted slot.
//
// For pragmatism + readability we collapse counts + scan into a
// single dispatch using a workgroup-cooperative scan, but the total
// is still ~150 lines.
//
// Implemented in radix_sort.comp.glsl through the C++ orchestrator
// (see gl_rasterizer.cpp). The shader source is below.
// ---------------------------------------------------------------------
inline constexpr const char* kGlslRadixHistogram = R"GLSL(
// One workgroup processes WG_SIZE * ITEMS_PER_THREAD = 1024 keys.
// Each workgroup writes its 256 bucket counts to the global histogram.
layout(local_size_x = 256) in;

layout(std430, binding = 3) readonly  buffer Keys    { uint64_t keys[]; };
layout(std430, binding = 5) writeonly buffer Hist    { uint hist[]; };

layout(location = 0) uniform uint N;
layout(location = 1) uniform uint shift;        // bits 0..56

shared uint local_hist[256];

void main() {
    uint tid = gl_LocalInvocationID.x;
    uint wg  = gl_WorkGroupID.x;
    local_hist[tid] = 0u;
    barrier();

    // 4 items per thread = 1024 per workgroup.
    for (uint it = 0u; it < 4u; ++it) {
        uint idx = wg * 1024u + it * 256u + tid;
        if (idx >= N) break;
        uint digit = uint((keys[idx] >> shift) & uint64_t(0xFFu));
        atomicAdd(local_hist[digit], 1u);
    }
    barrier();

    // Write this workgroup's histogram bin counts in row-major
    // [group * 256 + bucket] so the scan pass walks them contiguously
    // along the bucket dim.
    hist[wg * 256u + tid] = local_hist[tid];
}
)GLSL";

// ---------------------------------------------------------------------
// Scan over the (num_groups × 256) histogram. We need per-(group,
// bucket) prefix sums so that scattering knows where each key goes.
// We do it in two passes for simplicity:
//   pass A: per-bucket exclusive-prefix-sum down the group axis
//           (256 independent scans, one per bucket — `bucket_offsets`)
//   pass B: exclusive-prefix-sum across the bucket totals to get the
//           per-bucket BASE offset in the output array.
// Together: total offset for (group, bucket) = base[bucket] +
//           bucket_offsets[group, bucket].
//
// Implemented as one combined dispatch (we have at most 256 buckets
// and ~10k groups, so a 256-thread workgroup running a serial scan
// is fine).
// ---------------------------------------------------------------------
inline constexpr const char* kGlslRadixScan = R"GLSL(
layout(local_size_x = 256) in;

layout(std430, binding = 5) buffer Hist                { uint hist[]; };
layout(std430, binding = 7) writeonly buffer BucketBase { uint bucket_base[]; };

layout(location = 0) uniform uint num_groups;

shared uint bucket_totals[256];

void main() {
    uint bucket = gl_LocalInvocationID.x;

    // Pass A: scan down `hist[group, bucket]` for fixed bucket.
    // Convert in-place: bucket_offsets become exclusive prefix sums.
    uint sum = 0u;
    for (uint g = 0u; g < num_groups; ++g) {
        uint v = hist[g * 256u + bucket];
        hist[g * 256u + bucket] = sum;       // store exclusive prefix
        sum += v;
    }
    bucket_totals[bucket] = sum;
    barrier();

    // Pass B: thread 0 scans across the 256 buckets to get base offsets.
    if (bucket == 0u) {
        uint acc = 0u;
        for (uint b = 0u; b < 256u; ++b) {
            bucket_base[b] = acc;
            acc += bucket_totals[b];
        }
    }
}
)GLSL";

// ---------------------------------------------------------------------
// Scatter — re-read keys/values, compute each one's final slot, write
// to the *output* buffers. We use double-buffered key/value arrays:
// even passes read keys/values (3,4), write keys_alt/values_alt (8,9);
// odd passes swap. The host orchestrator handles ping-pong.
//
// For simplicity we use an additional per-(group, bucket) atomic
// counter (`hist`, since it now holds prefix sums) — atomicAdd
// returns the local slot inside this group's run, plus the bucket
// base.
// ---------------------------------------------------------------------
inline constexpr const char* kGlslRadixScatter = R"GLSL(
layout(local_size_x = 256) in;

layout(std430, binding = 3) readonly  buffer KeysIn     { uint64_t keys_in[]; };
layout(std430, binding = 4) readonly  buffer ValuesIn   { uint     values_in[]; };
layout(std430, binding = 8) writeonly buffer KeysOut    { uint64_t keys_out[]; };
layout(std430, binding = 9) writeonly buffer ValuesOut  { uint     values_out[]; };
layout(std430, binding = 5) buffer Hist                 { uint     hist[]; };
layout(std430, binding = 7) readonly buffer BucketBase  { uint     bucket_base[]; };

layout(location = 0) uniform uint N;
layout(location = 1) uniform uint shift;

void main() {
    uint tid = gl_LocalInvocationID.x;
    uint wg  = gl_WorkGroupID.x;

    for (uint it = 0u; it < 4u; ++it) {
        uint idx = wg * 1024u + it * 256u + tid;
        if (idx >= N) break;
        uint64_t key = keys_in[idx];
        uint val     = values_in[idx];
        uint digit   = uint((key >> shift) & uint64_t(0xFFu));
        // atomicAdd returns the local offset; the group's slot within
        // this digit's section starts at bucket_base[digit] + hist[wg,digit].
        uint local_slot = atomicAdd(hist[wg * 256u + digit], 1u);
        uint dst = bucket_base[digit] + local_slot;
        keys_out[dst]   = key;
        values_out[dst] = val;
    }
}
)GLSL";

// ---------------------------------------------------------------------
// Compute per-tile [start, end) in the sorted key array.
// ---------------------------------------------------------------------
inline constexpr const char* kGlslComputeRanges = R"GLSL(
layout(local_size_x = 256) in;

layout(std430, binding = 3) readonly  buffer Keys        { uint64_t keys[]; };
layout(std430, binding = 5) writeonly buffer TileRanges  { uvec2 tile_ranges[]; };

layout(location = 0) uniform uint total;
layout(location = 1) uniform uint num_tiles;

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= total) return;
    uint cur = uint(keys[i] >> 32);
    if (cur >= num_tiles) return;
    if (i == 0u) {
        tile_ranges[cur].x = 0u;
    } else {
        uint prev = uint(keys[i - 1u] >> 32);
        if (prev != cur) {
            if (prev < num_tiles) tile_ranges[prev].y = i;
            tile_ranges[cur].x = i;
        }
    }
    if (i == total - 1u) tile_ranges[cur].y = total;
}
)GLSL";

// ---------------------------------------------------------------------
// Rasterize — one workgroup (16x16 = 256 threads) per tile; each
// thread is one pixel. Front-to-back composite with T < MIN_T
// early-out. Direct port of the CUDA rasterize_kernel.
// ---------------------------------------------------------------------
inline constexpr const char* kGlslRasterize = R"GLSL(
layout(local_size_x = 16, local_size_y = 16) in;

layout(std430, binding = 1) readonly buffer Pre         { PreprocessedSplat pre[]; };
layout(std430, binding = 4) readonly buffer ValuesSorted { uint values_sorted[]; };
layout(std430, binding = 5) readonly buffer TileRanges  { uvec2 tile_ranges[]; };

layout(rgba8, binding = 0) writeonly uniform image2D out_image;

layout(location = 0) uniform int render_w;
layout(location = 1) uniform int render_h;
layout(location = 2) uniform int tile_grid_x;

shared PreprocessedSplat shared_splats[256];

void main() {
    uint tx = gl_WorkGroupID.x;
    uint ty = gl_WorkGroupID.y;
    uint px_x_in = gl_LocalInvocationID.x;
    uint px_y_in = gl_LocalInvocationID.y;
    ivec2 px = ivec2(tx * TILE_SIZE_X + px_x_in,
                     ty * TILE_SIZE_Y + px_y_in);
    bool inside = px.x < render_w && px.y < render_h;

    uint tile_id = ty * uint(tile_grid_x) + tx;
    uvec2 range = tile_ranges[tile_id];
    uint start = range.x;
    uint end   = range.y;
    uint total = end > start ? end - start : 0u;

    vec3 color = vec3(0.0);
    float T = 1.0;
    bool done = !inside;

    uint linear_tid = px_y_in * TILE_SIZE_X + px_x_in;
    uint batches = (total + 255u) / 256u;

    for (uint b = 0u; b < batches; ++b) {
        // Cooperative load of up to 256 splats into shared mem.
        uint fetch_idx = start + b * 256u + linear_tid;
        if (fetch_idx < end) {
            uint splat_id = values_sorted[fetch_idx];
            shared_splats[linear_tid] = pre[splat_id];
        }
        barrier();

        if (!done) {
            uint to_process = min(256u, total - b * 256u);
            for (uint k = 0u; k < to_process; ++k) {
                PreprocessedSplat s = shared_splats[k];
                vec2 d = vec2(float(px.x) + 0.5, float(px.y) + 0.5) - s.xy;
                float power = -0.5 * (s.conic.x * d.x * d.x +
                                      s.conic.z * d.y * d.y)
                              - s.conic.y * d.x * d.y;
                if (power > 0.0) continue;
                float w = exp(power);
                float a = min(MAX_ALPHA, s.alpha * w);
                if (a < (1.0/255.0)) continue;
                float weight = a * T;
                color += weight * s.color;
                T *= (1.0 - a);
                if (T < MIN_T) { done = true; break; }
            }
        }
        barrier();
    }

    if (inside) {
        vec4 pix = vec4(color, 1.0 - T);
        imageStore(out_image, px, pix);
    }
}
)GLSL";

// ---------------------------------------------------------------------
// Clear — fill the output image with (0,0,0,0).
// ---------------------------------------------------------------------
inline constexpr const char* kGlslClear = R"GLSL(
layout(local_size_x = 16, local_size_y = 16) in;
layout(rgba8, binding = 0) writeonly uniform image2D out_image;
layout(location = 0) uniform int w;
layout(location = 1) uniform int h;
void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (px.x < w && px.y < h) imageStore(out_image, px, vec4(0.0));
}
)GLSL";

}  // namespace gl_raster
