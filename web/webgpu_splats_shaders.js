/* WGSL shader sources for the WebGPU splat rasterizer.

   One-for-one ports of the GLSL compute shaders in
   gl_raster/shaders.h (desktop GL 4.3 compute build) — same algorithm,
   same bindings, same struct layouts. WGSL is enforced as the WebGPU
   shading language and intentionally similar to GLSL: most of the
   port is mechanical (`vec3<f32>` instead of `vec3`, `let`/`var`,
   `loop`/`for` syntax tweaks).

   Bindings (group 0):

     preprocess:
       0: storage<read>      splats[]        SplatGPU
       1: storage<write>     pre[]           PreprocessedSplat
       2: storage<write>     touched[]       u32
       3: uniform            Uniforms

     rasterize:
       0: storage<read>      pre[]
       1: storage<read>      values_sorted[] (sorted splat ids)
       2: storage<read>      tile_ranges[]   vec2<u32>
       3: storage texture    out_image       rgba8unorm
       4: uniform            Uniforms

     clear:
       0: storage texture    out_image
       1: uniform            Uniforms

     present (render):
       0: texture_2d<f32>    out_image
       1: sampler            samp
*/
(function () {
  'use strict';

  const PREAMBLE = `
struct SplatGPU {
  pos:     vec3<f32>,
  _p0:     f32,
  scales:  vec3<f32>,
  _p1:     f32,
  quat:    vec4<f32>,
  rgba:    vec4<f32>,
};

struct PreprocessedSplat {
  xy:        vec2<f32>,
  depth:     f32,
  radius:    f32,
  conic:     vec3<f32>,
  alpha:     f32,
  color:     vec3<f32>,
  _pad:      f32,
  tile_min:  vec2<u32>,
  tile_max:  vec2<u32>,
};

struct Uniforms {
  proj:                mat4x4<f32>,
  mesh_quat:           vec4<f32>,
  mesh_translate:      vec4<f32>,
  view_quat:           vec4<f32>,
  view_pos:            vec4<f32>,
  // 32: scalars packed as a vec4 to keep std140 alignment friendly:
  //  (mesh_scale, max_pixel_radius, blur_amount, _pad)
  scalars:             vec4<f32>,
  // 36: integer scalars
  //  (render_w, render_h, tile_grid_x, tile_grid_y)
  ints:                vec4<i32>,
  // 40: (N, _, _, _)
  ints2:               vec4<i32>,
};

fn quat_vec(q: vec4<f32>, v: vec3<f32>) -> vec3<f32> {
  let t = 2.0 * cross(q.xyz, v);
  return v + q.w * t + cross(q.xyz, t);
}

fn quat_quat(q1: vec4<f32>, q2: vec4<f32>) -> vec4<f32> {
  return vec4<f32>(
    q1.w*q2.x + q1.x*q2.w + q1.y*q2.z - q1.z*q2.y,
    q1.w*q2.y - q1.x*q2.z + q1.y*q2.w + q1.z*q2.x,
    q1.w*q2.z + q1.x*q2.y - q1.y*q2.x + q1.z*q2.w,
    q1.w*q2.w - q1.x*q2.x - q1.y*q2.y - q1.z*q2.z
  );
}

const TILE_X: u32 = 16u;
const TILE_Y: u32 = 16u;
const MAX_ALPHA: f32 = 0.99;
const MIN_T:     f32 = 1.0e-4;
`;

  const PREPROCESS_SRC = PREAMBLE + `
@group(0) @binding(0) var<storage, read>        splats:  array<SplatGPU>;
@group(0) @binding(1) var<storage, read_write>  pre:     array<PreprocessedSplat>;
@group(0) @binding(2) var<storage, read_write>  touched: array<u32>;
@group(0) @binding(3) var<uniform>              U:       Uniforms;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let i = gid.x;
  if (i32(i) >= U.ints2.x) { return; }

  let s = splats[i];
  let mesh_scale = U.scalars.x;

  // SplatTransformer
  var center = s.pos * mesh_scale;
  center = quat_vec(U.mesh_quat, center) + U.mesh_translate.xyz;
  let scales = s.scales * mesh_scale;
  let q = quat_quat(U.mesh_quat, s.quat);

  // High-α boost
  var alpha = s.rgba.a * 2.0;
  if (alpha > 1.0) {
    alpha = min(alpha * 4.0 - 3.0, 5.0);
  }

  let view_center = quat_vec(U.view_quat, center) + U.view_pos.xyz;
  if (view_center.z >= -0.2) {
    touched[i] = 0u; return;
  }

  let clip = U.proj * vec4<f32>(view_center, 1.0);
  if (abs(clip.z) >= clip.w) { touched[i] = 0u; return; }
  let clip_xy = 1.4 * clip.w;
  if (abs(clip.x) > clip_xy || abs(clip.y) > clip_xy) {
    touched[i] = 0u; return;
  }

  let ndc_x = clip.x / clip.w;
  let ndc_y = clip.y / clip.w;
  let px = (ndc_x * 0.5 + 0.5) * f32(U.ints.x);
  let py = (ndc_y * 0.5 + 0.5) * f32(U.ints.y);

  // View-space 3x3 cov = RS · RSᵀ
  let vq = quat_quat(U.view_quat, q);
  let r00 = 1.0 - 2.0 * (vq.y*vq.y + vq.z*vq.z);
  let r10 =        2.0 * (vq.x*vq.y + vq.w*vq.z);
  let r20 =        2.0 * (vq.x*vq.z - vq.w*vq.y);
  let r01 =        2.0 * (vq.x*vq.y - vq.w*vq.z);
  let r11 = 1.0 - 2.0 * (vq.x*vq.x + vq.z*vq.z);
  let r21 =        2.0 * (vq.y*vq.z + vq.w*vq.x);
  let r02 =        2.0 * (vq.x*vq.z + vq.w*vq.y);
  let r12 =        2.0 * (vq.y*vq.z - vq.w*vq.x);
  let r22 = 1.0 - 2.0 * (vq.x*vq.x + vq.y*vq.y);

  let RS00 = r00*scales.x; let RS01 = r01*scales.y; let RS02 = r02*scales.z;
  let RS10 = r10*scales.x; let RS11 = r11*scales.y; let RS12 = r12*scales.z;
  let RS20 = r20*scales.x; let RS21 = r21*scales.y; let RS22 = r22*scales.z;

  let c00 = RS00*RS00 + RS01*RS01 + RS02*RS02;
  let c01 = RS00*RS10 + RS01*RS11 + RS02*RS12;
  let c02 = RS00*RS20 + RS01*RS21 + RS02*RS22;
  let c11 = RS10*RS10 + RS11*RS11 + RS12*RS12;
  let c12 = RS10*RS20 + RS11*RS21 + RS12*RS22;
  let c22 = RS20*RS20 + RS21*RS21 + RS22*RS22;

  let fx = 0.5 * f32(U.ints.x) * U.proj[0][0];
  let fy = 0.5 * f32(U.ints.y) * U.proj[1][1];
  let invZ = 1.0 / view_center.z;
  let J00 = fx * invZ;
  let J02 = -fx * view_center.x * invZ * invZ;
  let J11 = fy * invZ;
  let J12 = -fy * view_center.y * invZ * invZ;

  let T00 = J00*c00 + J02*c02;
  let T01 = J00*c01 + J02*c12;
  let T02 = J00*c02 + J02*c22;
  let T10 = J11*c01 + J12*c02;
  let T11 = J11*c11 + J12*c12;
  let T12 = J11*c12 + J12*c22;

  let blur = U.scalars.z;
  let a = T00*J00 + T02*J02 + blur;
  let b = T01*J11 + T02*J12;
  let d = T11*J11 + T12*J12 + blur;

  let det = a * d - b * b;
  if (det <= 0.0) { touched[i] = 0u; return; }
  let inv_det = 1.0 / det;
  let conic = vec3<f32>(d*inv_det, -b*inv_det, a*inv_det);

  let mid = 0.5 * (a + d);
  let disc = max(0.0, mid*mid - det);
  let eig_max = mid + sqrt(disc);
  var radius = ceil(3.0 * sqrt(eig_max));
  if (radius > U.scalars.y) { radius = U.scalars.y; }
  if (radius < 1.0) { touched[i] = 0u; return; }

  let min_tx = max(0, i32(floor((px - radius) / f32(TILE_X))));
  let max_tx = min(U.ints.z - 1, i32(floor((px + radius) / f32(TILE_X))));
  let min_ty = max(0, i32(floor((py - radius) / f32(TILE_Y))));
  let max_ty = min(U.ints.w - 1, i32(floor((py + radius) / f32(TILE_Y))));
  if (max_tx < min_tx || max_ty < min_ty) {
    touched[i] = 0u; return;
  }

  pre[i].xy       = vec2<f32>(px, py);
  pre[i].depth    = -view_center.z;
  pre[i].radius   = radius;
  pre[i].conic    = conic;
  pre[i].alpha    = min(alpha, 1.0);
  pre[i].color    = s.rgba.rgb;
  pre[i].tile_min = vec2<u32>(u32(min_tx), u32(min_ty));
  pre[i].tile_max = vec2<u32>(u32(max_tx), u32(max_ty));

  touched[i] = u32((max_tx - min_tx + 1) * (max_ty - min_ty + 1));
}
`;

  // ── duplicate kernel ─────────────────────────────────────────────
  //
  // Per-splat, emit (key, value) pairs into a flat global array. The
  // key packs tile_id (13 bits) in the upper bits and the truncated
  // depth (19 bits) below — same layout the JS path was building
  // by hand. atomicAdd on emit_counter reserves `count` consecutive
  // slots for this splat; the actual tile loop writes into those
  // slots in tile-row-major order.
  //
  //   bindings:
  //     0: storage<read>      pre[]
  //     1: storage<read>      touched[]
  //     2: storage<read_write> keys[]       (uint32 packed)
  //     3: storage<read_write> values[]     (splat index uint32)
  //     4: storage<atomic_u32> emit_counter
  //     5: uniform            U
  //
  // The order of emit between splats is non-deterministic (atomic
  // race), but the host sort by key normalises that. Stable enough
  // for the rasterizer's front-to-back composite.
  const DUPLICATE_SRC = PREAMBLE + `
@group(0) @binding(0) var<storage, read>        pre:           array<PreprocessedSplat>;
@group(0) @binding(1) var<storage, read>        touched:       array<u32>;
@group(0) @binding(2) var<storage, read_write>  keys:          array<u32>;
@group(0) @binding(3) var<storage, read_write>  values:        array<u32>;
@group(0) @binding(4) var<storage, read_write>  emit_counter:  atomic<u32>;
@group(0) @binding(5) var<uniform>              U:             Uniforms;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let i = gid.x;
  if (i32(i) >= U.ints2.x) { return; }
  let count = touched[i];
  if (count == 0u) { return; }

  let ps = pre[i];
  let depth_bits = bitcast<u32>(ps.depth);
  let depth_high = depth_bits >> 13u;

  let off = atomicAdd(&emit_counter, count);
  var w: u32 = 0u;
  for (var ty: u32 = ps.tile_min.y; ty <= ps.tile_max.y; ty = ty + 1u) {
    let row_base = ty * u32(U.ints.z);
    for (var tx: u32 = ps.tile_min.x; tx <= ps.tile_max.x; tx = tx + 1u) {
      let tile_id = row_base + tx;
      keys[off + w]   = (tile_id << 19u) | depth_high;
      values[off + w] = i;
      w = w + 1u;
    }
  }
}
`;

  // ── radix sort ────────────────────────────────────────────────────
  //
  // 4-pass 8-bit radix sort over uint32 keys with a parallel uint32
  // value tagging along. Each pass = three dispatches:
  //
  //   histogram  — each workgroup processes 1024 keys (256 threads ×
  //                4 keys each), counts occurrences of each of 256
  //                possible 8-bit digit values in workgroup-shared
  //                memory, then writes the workgroup's bucket
  //                histogram into `hist[workgroup * 256 + bucket]`.
  //
  //   scan       — single workgroup of 256 threads. Each thread (=
  //                one bucket) walks down the per-group histogram
  //                column doing an exclusive prefix sum, and stores
  //                the bucket's grand total in workgroup memory.
  //                Then thread 0 prefix-sums the bucket totals to
  //                produce `bucket_base[256]` — the global write
  //                offset where each digit's run starts in the
  //                sorted output.
  //
  //   scatter    — each workgroup re-reads its 1024 keys and writes
  //                them (along with the parallel value) into
  //                `keys_out[bucket_base[digit] + hist[wg, digit]]`,
  //                atomicAdd'ing into hist[wg, digit] so siblings
  //                within the workgroup don't collide.
  //
  // The host orchestrator (web/webgpu_splats.js) ping-pongs the
  // (keys, values) buffers between two pairs and re-clears the hist
  // buffer between passes.
  //
  // Same shape as gl_raster/shaders.h on the desktop; the desktop
  // version had a ~1% mis-sort bug we never tracked down — we'll
  // see whether the WGSL version reproduces it.

  const SORT_HIST_SRC = `
struct SortU { N: u32, shift: u32, num_groups: u32, _pad: u32 };

@group(0) @binding(0) var<storage, read>       keys: array<u32>;
@group(0) @binding(1) var<storage, read_write> hist: array<u32>;
@group(0) @binding(2) var<uniform>             U:    SortU;

var<workgroup> shared_hist: array<atomic<u32>, 256>;

@compute @workgroup_size(256)
fn main(@builtin(local_invocation_id) lid: vec3<u32>,
        @builtin(workgroup_id)        wg:  vec3<u32>) {
  atomicStore(&shared_hist[lid.x], 0u);
  workgroupBarrier();

  for (var k: u32 = 0u; k < 4u; k = k + 1u) {
    let idx = wg.x * 1024u + k * 256u + lid.x;
    if (idx < U.N) {
      let digit = (keys[idx] >> U.shift) & 0xFFu;
      atomicAdd(&shared_hist[digit], 1u);
    }
  }
  workgroupBarrier();

  hist[wg.x * 256u + lid.x] = atomicLoad(&shared_hist[lid.x]);
}
`;

  const SORT_SCAN_SRC = `
struct SortU { N: u32, shift: u32, num_groups: u32, _pad: u32 };

@group(0) @binding(0) var<storage, read_write> hist:        array<u32>;
@group(0) @binding(1) var<storage, read_write> bucket_base: array<u32>;
@group(0) @binding(2) var<uniform>             U:           SortU;

var<workgroup> totals: array<u32, 256>;

@compute @workgroup_size(256)
fn main(@builtin(local_invocation_id) lid: vec3<u32>) {
  let bucket = lid.x;
  var sum: u32 = 0u;
  for (var g: u32 = 0u; g < U.num_groups; g = g + 1u) {
    let v = hist[g * 256u + bucket];
    hist[g * 256u + bucket] = sum;
    sum = sum + v;
  }
  totals[bucket] = sum;
  workgroupBarrier();

  if (bucket == 0u) {
    var acc: u32 = 0u;
    for (var b: u32 = 0u; b < 256u; b = b + 1u) {
      bucket_base[b] = acc;
      acc = acc + totals[b];
    }
  }
}
`;

  const SORT_SCATTER_SRC = `
struct SortU { N: u32, shift: u32, num_groups: u32, _pad: u32 };

@group(0) @binding(0) var<storage, read>       keys_in:     array<u32>;
@group(0) @binding(1) var<storage, read>       values_in:   array<u32>;
@group(0) @binding(2) var<storage, read_write> hist:        array<atomic<u32>>;
@group(0) @binding(3) var<storage, read>       bucket_base: array<u32>;
@group(0) @binding(4) var<storage, read_write> keys_out:    array<u32>;
@group(0) @binding(5) var<storage, read_write> values_out:  array<u32>;
@group(0) @binding(6) var<uniform>             U:           SortU;

@compute @workgroup_size(256)
fn main(@builtin(local_invocation_id) lid: vec3<u32>,
        @builtin(workgroup_id)        wg:  vec3<u32>) {
  for (var k: u32 = 0u; k < 4u; k = k + 1u) {
    let idx = wg.x * 1024u + k * 256u + lid.x;
    if (idx < U.N) {
      let key = keys_in[idx];
      let val = values_in[idx];
      let digit = (key >> U.shift) & 0xFFu;
      let local_slot = atomicAdd(&hist[wg.x * 256u + digit], 1u);
      let dst = bucket_base[digit] + local_slot;
      keys_out[dst]   = key;
      values_out[dst] = val;
    }
  }
}
`;

  // ── compute_ranges ────────────────────────────────────────────────
  //
  // After the sort, the keys are grouped by tile_id (top 13 bits).
  // For each entry in the sorted array, check if its tile_id differs
  // from the previous entry — if so, that's the start of a new
  // tile's run and the end of the previous tile's run. Writes
  // (start, end) pairs into the tile_ranges SSBO that the rasterize
  // kernel reads.
  const RANGES_SRC = `
struct RangesU { total: u32, num_tiles: u32, _p0: u32, _p1: u32 };

@group(0) @binding(0) var<storage, read>       sorted_keys: array<u32>;
@group(0) @binding(1) var<storage, read_write> tile_ranges: array<vec2<u32>>;
@group(0) @binding(2) var<uniform>             U:           RangesU;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let i = gid.x;
  if (i >= U.total) { return; }
  let cur = sorted_keys[i] >> 19u;
  if (cur >= U.num_tiles) { return; }
  if (i == 0u) {
    tile_ranges[cur].x = 0u;
  } else {
    let prev = sorted_keys[i - 1u] >> 19u;
    if (prev != cur) {
      if (prev < U.num_tiles) { tile_ranges[prev].y = i; }
      tile_ranges[cur].x = i;
    }
  }
  if (i == U.total - 1u) { tile_ranges[cur].y = U.total; }
}
`;

  const RASTERIZE_SRC = PREAMBLE + `
@group(0) @binding(0) var<storage, read>  pre:           array<PreprocessedSplat>;
@group(0) @binding(1) var<storage, read>  values_sorted: array<u32>;
@group(0) @binding(2) var<storage, read>  tile_ranges:   array<vec2<u32>>;
@group(0) @binding(3) var out_image: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(4) var<uniform> U: Uniforms;

var<workgroup> shared_splats: array<PreprocessedSplat, 256>;

@compute @workgroup_size(16, 16)
fn main(
  @builtin(workgroup_id) wg: vec3<u32>,
  @builtin(local_invocation_id) lid: vec3<u32>
) {
  let tx = wg.x; let ty = wg.y;
  let px = vec2<i32>(i32(tx * TILE_X + lid.x),
                     i32(ty * TILE_Y + lid.y));
  let inside = px.x < U.ints.x && px.y < U.ints.y;

  let tile_id = ty * u32(U.ints.z) + tx;
  let range = tile_ranges[tile_id];
  let start = range.x;
  let end   = range.y;
  let total = select(0u, end - start, end > start);

  var color = vec3<f32>(0.0);
  var T: f32 = 1.0;
  var done: bool = !inside;

  let linear_tid = lid.y * TILE_X + lid.x;
  let batches = (total + 255u) / 256u;

  for (var b: u32 = 0u; b < batches; b = b + 1u) {
    let fetch_idx = start + b * 256u + linear_tid;
    if (fetch_idx < end) {
      let splat_id = values_sorted[fetch_idx];
      shared_splats[linear_tid] = pre[splat_id];
    }
    workgroupBarrier();

    if (!done) {
      let to_process = min(256u, total - b * 256u);
      var k: u32 = 0u;
      loop {
        if (k >= to_process) { break; }
        let s = shared_splats[k];
        let d = vec2<f32>(f32(px.x) + 0.5, f32(px.y) + 0.5) - s.xy;
        let power = -0.5 * (s.conic.x * d.x * d.x +
                             s.conic.z * d.y * d.y)
                    - s.conic.y * d.x * d.y;
        if (power <= 0.0) {
          let w = exp(power);
          let a = min(MAX_ALPHA, s.alpha * w);
          if (a >= (1.0 / 255.0)) {
            let weight = a * T;
            color = color + weight * s.color;
            T = T * (1.0 - a);
            if (T < MIN_T) { done = true; break; }
          }
        }
        k = k + 1u;
      }
    }
    workgroupBarrier();
  }

  if (inside) {
    textureStore(out_image, px, vec4<f32>(color, 1.0 - T));
  }
}
`;

  const CLEAR_SRC = PREAMBLE + `
@group(0) @binding(0) var out_image: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(1) var<uniform> U: Uniforms;

@compute @workgroup_size(16, 16)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let p = vec2<i32>(i32(gid.x), i32(gid.y));
  if (p.x < U.ints.x && p.y < U.ints.y) {
    textureStore(out_image, p, vec4<f32>(0.0));
  }
}
`;

  // Present — a fullscreen pass that samples the storage texture
  // (rgba8unorm) and writes it to the canvas. WebGPU requires
  // storage textures be re-bound as `texture_2d<f32>` for sampling,
  // which works for rgba8unorm.
  const PRESENT_SRC = `
@group(0) @binding(0) var src:  texture_2d<f32>;
@group(0) @binding(1) var samp: sampler;

struct VOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vi: u32) -> VOut {
  // Fullscreen triangle.
  var p = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -3.0),
    vec2<f32>(-1.0,  1.0),
    vec2<f32>( 3.0,  1.0),
  );
  var u = array<vec2<f32>, 3>(
    vec2<f32>(0.0, 2.0),
    vec2<f32>(0.0, 0.0),
    vec2<f32>(2.0, 0.0),
  );
  var o: VOut;
  o.pos = vec4<f32>(p[vi], 0.0, 1.0);
  o.uv  = u[vi];
  return o;
}

@fragment
fn fs_main(@location(0) uv: vec2<f32>) -> @location(0) vec4<f32> {
  return textureSample(src, samp, uv);
}
`;

  window.WebGPUSplatsShaders = {
    modules(device) {
      return {
        preprocess:  device.createShaderModule({ code: PREPROCESS_SRC }),
        duplicate:   device.createShaderModule({ code: DUPLICATE_SRC }),
        sortHist:    device.createShaderModule({ code: SORT_HIST_SRC }),
        sortScan:    device.createShaderModule({ code: SORT_SCAN_SRC }),
        sortScatter: device.createShaderModule({ code: SORT_SCATTER_SRC }),
        ranges:      device.createShaderModule({ code: RANGES_SRC }),
        rasterize:   device.createShaderModule({ code: RASTERIZE_SRC }),
        clear:       device.createShaderModule({ code: CLEAR_SRC }),
        present:     device.createShaderModule({ code: PRESENT_SRC }),
      };
    },
  };
})();
