// C++ shim that bridges board_renderer.cpp's splat backdrop path to
// the WebGPU-on-its-own-canvas rasterizer implemented in
// web/webgpu_splats.js. Web-only — desktop builds use
// gl_raster/gl_rasterizer.cpp directly.
//
// Activation: append `?webgpu` to the URL or set
// `localStorage.CHESS_WEBGPU_SPLATS = "1"`. The init pass detects
// support at startup; if WebGPU isn't available (e.g. Firefox
// without the flag, older Safari), the activation flag is ignored
// and the chess web build keeps its existing per-quad path.

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "../splat.h"

// Tri-state init status: 0 = uninit, 1 = initialising, 2 = ready,
// -1 = not supported. Read by chess_webgpu_splats_supported() so the
// chess renderer can fall back to the per-quad path if WebGPU isn't
// available or the activation flag isn't set.
static int g_init_state = 0;

extern "C" {

// Called once at startup from main_web.cpp. Returns immediately —
// the actual WebGPU init is async; further calls poll
// chess_webgpu_splats_supported() until it returns 2 or -1.
EM_JS(void, _chess_webgpu_init, (), {
  const url = new URL(window.location);
  const want = url.searchParams.has('webgpu') ||
               url.hash.includes('webgpu') ||
               (window.localStorage &&
                window.localStorage.getItem('CHESS_WEBGPU_SPLATS') === '1');
  if (!want) { Module._chess_webgpu_set_state(-1); return; }
  if (!window.WebGPUSplats || !window.WebGPUSplats.supported()) {
    console.warn('[webgpu-splats] not supported in this browser');
    Module._chess_webgpu_set_state(-1);
    return;
  }
  Module._chess_webgpu_set_state(1);  // initialising
  window.WebGPUSplats.init().then(ok => {
    if (ok) {
      console.log('[webgpu-splats] enabled');
      Module._chess_webgpu_set_state(2);
    } else {
      console.warn('[webgpu-splats] init failed');
      Module._chess_webgpu_set_state(-1);
    }
  }).catch(err => {
    console.warn('[webgpu-splats] init error:', err);
    Module._chess_webgpu_set_state(-1);
  });
});

// Setter exposed so the async JS init path can write the result
// back into our tri-state.
EMSCRIPTEN_KEEPALIVE void chess_webgpu_set_state(int s) {
  g_init_state = s;
}

EMSCRIPTEN_KEEPALIVE int chess_webgpu_supported() {
  return g_init_state == 2 ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int chess_webgpu_init_state() {
  return g_init_state;
}

// Upload the SplatGPU-layout buffer (already flat float[]) to
// WebGPU. Caller pre-packs from std::vector<Splat>.
EM_JS(void, _chess_webgpu_upload, (uintptr_t fbuf, int n, int flip_y), {
  if (!window.WebGPUSplats || !window.WebGPUSplats.device) return;
  // Re-slice into a TypedArray view of WASM memory.
  const arr = new Float32Array(HEAPU8.buffer, fbuf, n * 16);
  // upload() mutates in place for flipY; make a copy so we don't
  // corrupt the WASM heap.
  const copy = new Float32Array(arr);
  window.WebGPUSplats.upload(copy, n, flip_y !== 0);
});

EM_JS(void, _chess_webgpu_resize, (int w, int h), {
  if (!window.WebGPUSplats || !window.WebGPUSplats.device) return;
  window.WebGPUSplats.resize(w, h);
});

EM_JS(void, _chess_webgpu_set_visible, (int v), {
  if (!window.WebGPUSplats) return;
  window.WebGPUSplats.setVisible(v !== 0);
});

// `view`/`proj`/`model` are pointers into WASM memory at 16 floats
// each. Render dispatches the compute pipelines. Returns
// immediately; the WebGPU work is queued on the device.
EM_JS(void, _chess_webgpu_render, (uintptr_t v, uintptr_t p, uintptr_t m), {
  if (!window.WebGPUSplats || !window.WebGPUSplats.device) return;
  if (!window.WebGPUSplats.visible) return;
  const view  = new Float32Array(HEAPU8.buffer, v, 16);
  const proj  = new Float32Array(HEAPU8.buffer, p, 16);
  const model = new Float32Array(HEAPU8.buffer, m, 16);
  // render is async (awaits the touched-buffer readback) — fire
  // and forget so we don't block the JS main loop. The next frame's
  // render() will skip if the previous one is still in flight.
  if (!window.WebGPUSplats._inflight) {
    window.WebGPUSplats._inflight = true;
    window.WebGPUSplats.render(view, proj, model).finally(() => {
      window.WebGPUSplats._inflight = false;
    });
  }
});

// Public C entry points used by board_renderer.cpp.

EMSCRIPTEN_KEEPALIVE void chess_webgpu_init() { _chess_webgpu_init(); }

EMSCRIPTEN_KEEPALIVE void chess_webgpu_upload_splats(const Splat* splats,
                                                     int n, int flip_y) {
  // Pack into the std430-equivalent layout matching SplatGPU in
  // webgpu_splats.js (float pos[3]+pad, float scales[3]+pad,
  // float quat[4], float rgba[4]; 64 bytes total).
  std::vector<float> buf(n * 16, 0.0f);
  for (int i = 0; i < n; ++i) {
    const Splat& s = splats[i];
    float* g = buf.data() + i * 16;
    g[0] = s.pos[0]; g[1] = s.pos[1]; g[2] = s.pos[2];
    g[4] = s.scales[0]; g[5] = s.scales[1]; g[6] = s.scales[2];
    g[8]  = float(s.quat[0]);
    g[9]  = float(s.quat[1]);
    g[10] = float(s.quat[2]);
    g[11] = float(s.quat[3]);
    g[12] = s.rgba[0] / 255.0f;
    g[13] = s.rgba[1] / 255.0f;
    g[14] = s.rgba[2] / 255.0f;
    g[15] = s.rgba[3] / 255.0f;
  }
  _chess_webgpu_upload(reinterpret_cast<uintptr_t>(buf.data()), n, flip_y);
}

EMSCRIPTEN_KEEPALIVE void chess_webgpu_resize(int w, int h) {
  _chess_webgpu_resize(w, h);
}

EMSCRIPTEN_KEEPALIVE void chess_webgpu_set_visible(int v) {
  _chess_webgpu_set_visible(v);
}

EMSCRIPTEN_KEEPALIVE void chess_webgpu_render(const float* view,
                                              const float* proj,
                                              const float* model) {
  _chess_webgpu_render(reinterpret_cast<uintptr_t>(view),
                       reinterpret_cast<uintptr_t>(proj),
                       reinterpret_cast<uintptr_t>(model));
}

}  // extern "C"

#endif  // __EMSCRIPTEN__
