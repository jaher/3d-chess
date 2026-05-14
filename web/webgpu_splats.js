/* WebGPU tile-based gsplat rasterizer — web counterpart of the
   desktop gl_raster/ compute path.

   WebGL2 (what the rest of the chess web build uses) has no compute
   shaders, so we keep the existing per-quad path on the WebGL
   canvas and bolt a second, WebGPU-only canvas underneath it for
   the splat backdrop. The chess WebGL canvas is made alpha=true +
   premultiplied so anywhere it doesn't draw (background area) the
   WebGPU canvas shows through.

   Same algorithm as the desktop GL compute path:
     1. preprocess  — per-splat projection + cov2D + tile bbox
     2. duplicate   — emit (tile_id, depth, splat_id) tuples
     3. sort        — sort by (tile_id, depth) ascending (per-tile
                      front-to-back). Done on CPU with Uint32Array
                      indirect indices (slow vs GPU radix, but Safari
                      doesn't ship GPU radix sort and the camera
                      hardly ever moves — the chess scene is mostly
                      static, so the sort runs once per camera change).
     4. compute_ranges — find per-tile [start, end) ranges
     5. rasterize   — one workgroup per 16×16 tile; per-pixel
                      front-to-back composite

   Exposed surface (used by web/webgpu_splats_web.cpp via EM_JS):
     WebGPUSplats.supported()          → bool
     WebGPUSplats.init()               → async; creates device + canvas
     WebGPUSplats.upload(splatBuffer, n, flipY)
                                       → upload N splats (host-flat
                                          float layout matching SplatGPU)
     WebGPUSplats.resize(w, h)         → resize backbuffer + tile grid
     WebGPUSplats.render(view, proj, model)
                                       → dispatch compute pipelines
     WebGPUSplats.setVisible(v)        → show/hide the backdrop canvas
*/
(function () {
  'use strict';

  const TILE_SIZE = 16;

  // SplatGPU std430 layout matches desktop gl_raster/gl_rasterizer.cpp
  // and shaders.h. 64 bytes per splat.
  const SPLAT_STRIDE = 64;

  // PreprocessedSplat — 64 bytes per splat (kept in lockstep with the
  // GLSL std430 struct so the desktop + web shaders can compute the
  // exact same per-splat output).
  const PRE_STRIDE = 64;

  const WebGPUSplats = {
    device: null,
    canvas: null,
    canvasContext: null,
    canvasFormat: null,
    splatCount: 0,
    width: 0,
    height: 0,
    tileGridX: 0,
    tileGridY: 0,
    pipelines: {},      // preprocess, duplicate, computeRanges, rasterize, clear
    buffers: {},        // splats, pre, touched, keys, values, tileRanges, uniforms
    bindGroups: {},
    visible: false,

    supported() {
      return typeof navigator !== 'undefined'
          && !!navigator.gpu
          && typeof GPUDevice !== 'undefined';
    },

    async init() {
      // Each failure path logs exactly where it bailed so the user
      // can paste the line into a bug report — the bare
      // "[webgpu-splats] init failed" from the outer callback
      // doesn't say which step dropped.
      if (!this.supported()) {
        console.warn('[webgpu-splats] supported() returned false ' +
                     '(navigator.gpu missing?)');
        return false;
      }
      try {
        // First try the hardware adapter (high-perf, discrete GPU
        // on dual-GPU systems).
        let adapter = await navigator.gpu.requestAdapter(
            { powerPreference: 'high-performance' });
        // On Linux Chrome without `chrome://flags/#enable-unsafe-webgpu`
        // the hardware adapter returns null. Fall back to the
        // software (Dawn/SwiftShader) adapter — slow but it at
        // least proves the pipeline + WGSL compile path works.
        if (!adapter) {
          console.warn('[webgpu-splats] hardware adapter null — ' +
                       'trying software fallback. For full speed ' +
                       'enable chrome://flags/#enable-unsafe-webgpu ' +
                       'and restart Chrome.');
          adapter = await navigator.gpu.requestAdapter(
              { forceFallbackAdapter: true });
        }
        if (!adapter) {
          console.warn('[webgpu-splats] no adapter (hardware or ' +
                       'software). WebGPU is unavailable in this ' +
                       'browser/profile. Enable ' +
                       'chrome://flags/#enable-unsafe-webgpu and ' +
                       'restart Chrome.');
          return false;
        }
        console.log('[webgpu-splats] adapter info:',
                    adapter.info || adapter);
        this.device = await adapter.requestDevice();
        console.log('[webgpu-splats] device created');
      } catch (e) {
        console.warn('[webgpu-splats] adapter/device init threw:', e);
        return false;
      }
      // Create the backdrop canvas, behind the chess WebGL canvas.
      try {
        const wrap = document.getElementById('canvas-wrap');
        const c = document.createElement('canvas');
        c.id = 'webgpu-backdrop';
        c.style.position = 'absolute';
        c.style.top = '0';
        c.style.left = '0';
        c.style.width = '100%';
        c.style.height = '100%';
        c.style.zIndex = '0';
        c.style.pointerEvents = 'none';
        c.style.display = 'none';
        wrap.insertBefore(c, wrap.firstChild);
        this.canvas = c;
        const ctx = c.getContext('webgpu');
        if (!ctx) {
          console.warn('[webgpu-splats] canvas.getContext("webgpu") returned null');
          return false;
        }
        this.canvasContext = ctx;
        this.canvasFormat = navigator.gpu.getPreferredCanvasFormat();
        ctx.configure({
          device: this.device,
          format: this.canvasFormat,
          alphaMode: 'premultiplied',
        });
      } catch (e) {
        console.warn('[webgpu-splats] canvas/context setup threw:', e);
        return false;
      }
      try {
        this._buildPipelines();
      } catch (e) {
        console.warn('[webgpu-splats] _buildPipelines threw:', e);
        return false;
      }
      console.log('[webgpu-splats] init ok, canvasFormat=', this.canvasFormat);
      return true;
    },

    setVisible(v) {
      this.visible = !!v;
      if (this.canvas) this.canvas.style.display = v ? 'block' : 'none';
      // Body class drives the CSS that makes the WebGL chess canvas
      // background-transparent so the WebGPU canvas behind shows
      // through.
      document.body.classList.toggle('webgpu-splats', !!v);
    },

    // Debug helper — call WebGPUSplats.soloMode(true) from the
    // DevTools console to PROMOTE the WebGPU canvas to z-index 100
    // (above the chess WebGL canvas) so the splat backdrop covers
    // the chess view. Call with false to restore. Useful to verify
    // that the WGSL compute pipeline is actually producing
    // tile-composited output — the chess camera frames the splats
    // from outside the room, where per-quad and tile-based output
    // look similar.
    soloMode(on) {
      if (!this.canvas) return;
      this.canvas.style.zIndex = on ? '100' : '0';
    },

    resize(w, h) {
      if (!this.canvas || (w === this.width && h === this.height)) return;
      this.canvas.width = w;
      this.canvas.height = h;
      this.width = w;
      this.height = h;
      this.tileGridX = Math.ceil(w / TILE_SIZE);
      this.tileGridY = Math.ceil(h / TILE_SIZE);
      // Per-tile output image — written by the rasterize compute
      // shader, presented by a small render pass to the canvas.
      if (this.buffers.outImage) this.buffers.outImage.destroy();
      this.buffers.outImage = this.device.createTexture({
        size: [w, h],
        format: 'rgba8unorm',
        usage: GPUTextureUsage.STORAGE_BINDING | GPUTextureUsage.TEXTURE_BINDING,
      });
      // Tile ranges buffer.
      if (this.buffers.tileRanges) this.buffers.tileRanges.destroy();
      this.buffers.tileRanges = this.device.createBuffer({
        size: this.tileGridX * this.tileGridY * 8,  // uvec2 per tile
        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
      });
    },

    // splatBuffer: Float32Array of N * 16 floats (SplatGPU layout —
    // pos[3]+pad, scales[3]+pad, quat[4], rgba[4]).
    upload(splatBuffer, n, flipY) {
      this.splatCount = n;
      if (flipY) {
        for (let i = 0; i < n; ++i) {
          const off = i * 16;
          splatBuffer[off + 1] = -splatBuffer[off + 1];          // pos.y
          splatBuffer[off + 8]  = -splatBuffer[off + 8];          // quat.x
          splatBuffer[off + 10] = -splatBuffer[off + 10];         // quat.z
        }
      }
      if (this.buffers.splats) this.buffers.splats.destroy();
      this.buffers.splats = this.device.createBuffer({
        size: n * SPLAT_STRIDE,
        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
      });
      this.device.queue.writeBuffer(this.buffers.splats, 0, splatBuffer);

      if (this.buffers.pre) this.buffers.pre.destroy();
      this.buffers.pre = this.device.createBuffer({
        size: n * PRE_STRIDE,
        // COPY_SRC: render() reads tile_min/max + depth back to CPU
        // to build the sort keys (no GPU radix sort yet).
        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC,
      });
      if (this.buffers.touched) this.buffers.touched.destroy();
      this.buffers.touched = this.device.createBuffer({
        size: n * 4,
        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC,
      });
      // Reusable staging buffer for the touched-tile readback (we
      // re-map on every frame; WebGPU forbids reusing a mapped buf
      // without unmapping first).
      if (this.buffers.touchedReadback) this.buffers.touchedReadback.destroy();
      this.buffers.touchedReadback = this.device.createBuffer({
        size: n * 4,
        usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
      });
      console.log('[webgpu-splats] uploaded', n, 'splats =',
                  (n * SPLAT_STRIDE / 1024 / 1024).toFixed(1), 'MB');
    },

    // view, proj, model: Float32Array(16) each, column-major.
    async render(view, proj, model) {
      if (!this.device || !this.splatCount || !this.visible) return;
      if (!this.width || !this.height) return;
      // Shoemake decomposition done on CPU (host side) so the kernel
      // can stay simple. The chess Y-flip is already baked into the
      // splat data via upload(... flipY=true) so we pass a positive-
      // Y model matrix here.
      const ms = shoemakeDecompose(model);
      const vs = shoemakeDecompose(view);
      // Uniforms layout (std140-friendly, 256-byte aligned because
      // WebGPU requires alignment for uniform/storage binding offsets).
      const ubo = new ArrayBuffer(256);
      const f = new Float32Array(ubo);
      const i32 = new Int32Array(ubo);
      // 0..16: proj mat4 (column-major)
      f.set(proj, 0);
      // 16..19: mesh_quat
      f[16] = ms.quat[0]; f[17] = ms.quat[1];
      f[18] = ms.quat[2]; f[19] = ms.quat[3];
      // 20..23: mesh_translate (vec4)
      f[20] = model[12]; f[21] = model[13]; f[22] = model[14]; f[23] = 0;
      // 24..27: view_quat
      f[24] = vs.quat[0]; f[25] = vs.quat[1];
      f[26] = vs.quat[2]; f[27] = vs.quat[3];
      // 28..31: view_pos (vec4)
      f[28] = view[12]; f[29] = view[13]; f[30] = view[14]; f[31] = 0;
      // 32: mesh_scale
      f[32] = ms.scale;
      // 33: max_pixel_radius — bumped from 96 → 256 so larger
      // splats actually contribute their full Gaussian footprint to
      // the per-pixel composite. At the chess camera the room
      // walls are at moderate distance: 96 was clipping splats to
      // tight discs and the tile-based output read as essentially
      // identical to the per-quad WebGL backdrop. 256 lets the
      // front-to-back composite show its quality advantage
      // (softer falloff, less smearing in overlap regions).
      // Total tile-touch count goes up ~7× (radius²), still
      // tractable under the packed-uint32 JS sort path.
      f[33] = 256.0;
      // 34: blur_amount
      f[34] = 0.3;
      // 35..: ints (render_w, render_h, tile_gx, tile_gy, N)
      i32[36] = this.width;
      i32[37] = this.height;
      i32[38] = this.tileGridX;
      i32[39] = this.tileGridY;
      i32[40] = this.splatCount;
      if (!this.buffers.uniforms) {
        this.buffers.uniforms = this.device.createBuffer({
          size: 256,
          usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
        });
      }
      this.device.queue.writeBuffer(this.buffers.uniforms, 0, ubo);

      // Bind groups for each pipeline.
      const bg = this._bindGroups();
      const enc = this.device.createCommandEncoder();

      // Clear output image
      {
        const p = enc.beginComputePass();
        p.setPipeline(this.pipelines.clear);
        p.setBindGroup(0, bg.clear);
        p.dispatchWorkgroups(
            Math.ceil(this.width / 16),
            Math.ceil(this.height / 16));
        p.end();
      }

      // Preprocess
      {
        const p = enc.beginComputePass();
        p.setPipeline(this.pipelines.preprocess);
        p.setBindGroup(0, bg.preprocess);
        p.dispatchWorkgroups(Math.ceil(this.splatCount / 256));
        p.end();
      }

      // Copy touched[] back so we can prefix-sum + sort on CPU.
      enc.copyBufferToBuffer(this.buffers.touched, 0,
                              this.buffers.touchedReadback, 0,
                              this.splatCount * 4);
      this.device.queue.submit([enc.finish()]);

      // CPU readback + sort. Async.
      await this.buffers.touchedReadback.mapAsync(GPUMapMode.READ);
      const touched = new Uint32Array(
          this.buffers.touchedReadback.getMappedRange().slice(0));
      this.buffers.touchedReadback.unmap();

      let total = 0;
      const offsets = new Uint32Array(this.splatCount);
      for (let i = 0; i < this.splatCount; ++i) {
        offsets[i] = total;
        total += touched[i];
      }
      if (total === 0) {
        // Nothing visible — present the cleared canvas.
        this._present();
        return;
      }

      // ── duplicate on GPU: each splat emits its (key, value) pairs
      //    into shared keys[]/values[] arrays via an atomic emit
      //    counter (see DUPLICATE_SRC in webgpu_splats_shaders.js).
      //    Replaces the previous JS loop that iterated 500k splats ×
      //    avg N tile-touches and built BigUint64-style keys by
      //    hand — that loop was the dominant per-frame cost.
      const totalBytes = total * 4;
      if (this.buffers.keys && this.buffers.keys.size < totalBytes) {
        this.buffers.keys.destroy(); this.buffers.keys = null;
        if (this.buffers.values) { this.buffers.values.destroy(); this.buffers.values = null; }
        if (this.buffers.keysReadback) { this.buffers.keysReadback.destroy(); this.buffers.keysReadback = null; }
        if (this.buffers.valuesReadback) { this.buffers.valuesReadback.destroy(); this.buffers.valuesReadback = null; }
      }
      if (!this.buffers.keys) {
        this.buffers.keys = this.device.createBuffer({
          size: totalBytes,
          usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC,
        });
        this.buffers.values = this.device.createBuffer({
          size: totalBytes,
          usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC,
        });
        this.buffers.keysReadback = this.device.createBuffer({
          size: totalBytes,
          usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
        });
        this.buffers.valuesReadback = this.device.createBuffer({
          size: totalBytes,
          usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
        });
      }
      if (!this.buffers.emitCounter) {
        this.buffers.emitCounter = this.device.createBuffer({
          size: 4,
          usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
        });
      }
      // Reset emit counter to 0.
      this.device.queue.writeBuffer(this.buffers.emitCounter, 0,
                                    new Uint32Array([0]));

      // Build duplicate bind group + dispatch.
      const dupBg = this.device.createBindGroup({
        layout: this.pipelines.duplicate.getBindGroupLayout(0),
        entries: [
          { binding: 0, resource: { buffer: this.buffers.pre } },
          { binding: 1, resource: { buffer: this.buffers.touched } },
          { binding: 2, resource: { buffer: this.buffers.keys } },
          { binding: 3, resource: { buffer: this.buffers.values } },
          { binding: 4, resource: { buffer: this.buffers.emitCounter } },
          { binding: 5, resource: { buffer: this.buffers.uniforms } },
        ],
      });
      const encDup = this.device.createCommandEncoder();
      {
        const p = encDup.beginComputePass();
        p.setPipeline(this.pipelines.duplicate);
        p.setBindGroup(0, dupBg);
        p.dispatchWorkgroups(Math.ceil(this.splatCount / 256));
        p.end();
      }
      // Stage the duplicate output for readback in the same submit
      // so we get it on the next mapAsync.
      encDup.copyBufferToBuffer(this.buffers.keys, 0,
                                this.buffers.keysReadback, 0,
                                totalBytes);
      encDup.copyBufferToBuffer(this.buffers.values, 0,
                                this.buffers.valuesReadback, 0,
                                totalBytes);
      this.device.queue.submit([encDup.finish()]);

      await Promise.all([
        this.buffers.keysReadback.mapAsync(GPUMapMode.READ),
        this.buffers.valuesReadback.mapAsync(GPUMapMode.READ),
      ]);
      const keys = new Uint32Array(
          this.buffers.keysReadback.getMappedRange().slice(0));
      const values = new Uint32Array(
          this.buffers.valuesReadback.getMappedRange().slice(0));
      this.buffers.keysReadback.unmap();
      this.buffers.valuesReadback.unmap();

      // Sort the (key, value) pairs by key. TypedArray.sort with a
      // numeric comparator is the fast path; we sort an index array
      // to keep keys + values in sync.
      const tg = this.tileGridX;
      const idx = new Uint32Array(total);
      for (let i = 0; i < total; ++i) idx[i] = i;
      idx.sort((a, b) => keys[a] - keys[b]);
      const sortedKeys = new Uint32Array(total);
      const sortedValues = new Uint32Array(total);
      for (let i = 0; i < total; ++i) {
        sortedKeys[i]   = keys[idx[i]];
        sortedValues[i] = values[idx[i]];
      }

      // Build tile ranges on CPU.
      const nTiles = tg * this.tileGridY;
      const ranges = new Uint32Array(nTiles * 2);   // (start, end) pairs
      for (let i = 0; i < total; ++i) {
        const tid = sortedKeys[i] >>> 19;
        if (tid >= nTiles) continue;
        if (i === 0) ranges[tid * 2] = 0;
        else {
          const prev = sortedKeys[i - 1] >>> 19;
          if (prev !== tid) {
            if (prev < nTiles) ranges[prev * 2 + 1] = i;
            ranges[tid * 2] = i;
          }
        }
        if (i === total - 1) ranges[tid * 2 + 1] = total;
      }

      // Upload sorted values + tile ranges + dispatch rasterize.
      if (this.buffers.valuesSorted &&
          this.buffers.valuesSorted.size < total * 4) {
        this.buffers.valuesSorted.destroy();
        this.buffers.valuesSorted = null;
      }
      if (!this.buffers.valuesSorted) {
        this.buffers.valuesSorted = this.device.createBuffer({
          size: total * 4,
          usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
        });
      }
      this.device.queue.writeBuffer(this.buffers.valuesSorted, 0, sortedValues);
      this.device.queue.writeBuffer(this.buffers.tileRanges, 0, ranges);

      const enc3 = this.device.createCommandEncoder();
      {
        const p = enc3.beginComputePass();
        p.setPipeline(this.pipelines.rasterize);
        p.setBindGroup(0, bg.rasterizeBindGroup());
        p.dispatchWorkgroups(this.tileGridX, this.tileGridY);
        p.end();
      }
      this.device.queue.submit([enc3.finish()]);

      this._present();
    },

    _present() {
      // Blit out_image → canvas via a simple render pass.
      if (!this.pipelines.present) return;
      const enc = this.device.createCommandEncoder();
      const view = this.canvasContext.getCurrentTexture().createView();
      const p = enc.beginRenderPass({
        colorAttachments: [{
          view,
          loadOp: 'clear', storeOp: 'store',
          clearValue: { r: 0, g: 0, b: 0, a: 0 },
        }],
      });
      p.setPipeline(this.pipelines.present);
      p.setBindGroup(0, this._presentBindGroup());
      p.draw(3);
      p.end();
      this.device.queue.submit([enc.finish()]);
    },

    _bindGroups() {
      // Bind groups need to be re-created when buffer handles change
      // (resize / upload). Cheap to rebuild — group layouts are
      // pipeline-internal.
      const dev = this.device;
      return {
        clear: dev.createBindGroup({
          layout: this.pipelines.clear.getBindGroupLayout(0),
          entries: [
            { binding: 0, resource: this.buffers.outImage.createView() },
            { binding: 1, resource: { buffer: this.buffers.uniforms } },
          ],
        }),
        preprocess: dev.createBindGroup({
          layout: this.pipelines.preprocess.getBindGroupLayout(0),
          entries: [
            { binding: 0, resource: { buffer: this.buffers.splats } },
            { binding: 1, resource: { buffer: this.buffers.pre } },
            { binding: 2, resource: { buffer: this.buffers.touched } },
            { binding: 3, resource: { buffer: this.buffers.uniforms } },
          ],
        }),
        // Built lazily because valuesSorted's size changes.
        rasterizeBindGroup: () => dev.createBindGroup({
          layout: this.pipelines.rasterize.getBindGroupLayout(0),
          entries: [
            { binding: 0, resource: { buffer: this.buffers.pre } },
            { binding: 1, resource: { buffer: this.buffers.valuesSorted } },
            { binding: 2, resource: { buffer: this.buffers.tileRanges } },
            { binding: 3, resource: this.buffers.outImage.createView() },
            { binding: 4, resource: { buffer: this.buffers.uniforms } },
          ],
        }),
      };
    },

    _presentBindGroup() {
      return this.device.createBindGroup({
        layout: this.pipelines.present.getBindGroupLayout(0),
        entries: [
          { binding: 0, resource: this.buffers.outImage.createView() },
          { binding: 1, resource: this.device.createSampler({
              magFilter: 'linear', minFilter: 'linear' }) },
        ],
      });
    },

    _buildPipelines() {
      const dev = this.device;
      const shaderModules = WebGPUSplatsShaders.modules(dev);
      this.pipelines.clear = dev.createComputePipeline({
        layout: 'auto',
        compute: { module: shaderModules.clear, entryPoint: 'main' },
      });
      this.pipelines.preprocess = dev.createComputePipeline({
        layout: 'auto',
        compute: { module: shaderModules.preprocess, entryPoint: 'main' },
      });
      this.pipelines.duplicate = dev.createComputePipeline({
        layout: 'auto',
        compute: { module: shaderModules.duplicate, entryPoint: 'main' },
      });
      this.pipelines.rasterize = dev.createComputePipeline({
        layout: 'auto',
        compute: { module: shaderModules.rasterize, entryPoint: 'main' },
      });
      this.pipelines.present = dev.createRenderPipeline({
        layout: 'auto',
        vertex:   { module: shaderModules.present, entryPoint: 'vs_main' },
        fragment: { module: shaderModules.present, entryPoint: 'fs_main',
                    targets: [{ format: this.canvasFormat }] },
        primitive: { topology: 'triangle-list' },
      });
    },
  };

  // ── Shoemake quaternion decomposition from a column-major mat4
  //    (chosen to match the C++ host-side helpers exactly so the
  //    WGSL preprocess kernel can stay rotation-only). ──
  function shoemakeDecompose(m) {
    const sx = Math.hypot(m[0], m[1], m[2]);
    const sy = Math.hypot(m[4], m[5], m[6]);
    const sz = Math.hypot(m[8], m[9], m[10]);
    const isx = sx > 1e-8 ? 1.0 / sx : 0.0;
    const isy = sy > 1e-8 ? 1.0 / sy : 0.0;
    const isz = sz > 1e-8 ? 1.0 / sz : 0.0;
    const r00 = m[0]*isx, r10 = m[1]*isx, r20 = m[2]*isx;
    const r01 = m[4]*isy, r11 = m[5]*isy, r21 = m[6]*isy;
    const r02 = m[8]*isz, r12 = m[9]*isz, r22 = m[10]*isz;
    const tr = r00 + r11 + r22;
    const q = [0, 0, 0, 1];
    if (tr > 0) {
      const s = 0.5 / Math.sqrt(tr + 1);
      q[3] = 0.25 / s;
      q[0] = (r21 - r12) * s;
      q[1] = (r02 - r20) * s;
      q[2] = (r10 - r01) * s;
    } else if (r00 > r11 && r00 > r22) {
      const s = 2 * Math.sqrt(1 + r00 - r11 - r22);
      q[3] = (r21 - r12) / s;
      q[0] = 0.25 * s;
      q[1] = (r01 + r10) / s;
      q[2] = (r02 + r20) / s;
    } else if (r11 > r22) {
      const s = 2 * Math.sqrt(1 + r11 - r00 - r22);
      q[3] = (r02 - r20) / s;
      q[0] = (r01 + r10) / s;
      q[1] = 0.25 * s;
      q[2] = (r12 + r21) / s;
    } else {
      const s = 2 * Math.sqrt(1 + r22 - r00 - r11);
      q[3] = (r10 - r01) / s;
      q[0] = (r02 + r20) / s;
      q[1] = (r12 + r21) / s;
      q[2] = 0.25 * s;
    }
    return { scale: (sx + sy + sz) / 3, quat: q };
  }

  // Globals.
  window.WebGPUSplats = WebGPUSplats;
})();
