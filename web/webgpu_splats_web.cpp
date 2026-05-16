// C++ shim that bridges board_renderer.cpp's splat backdrop path to
// the WebGPU-on-its-own-canvas rasterizer implemented in
// web/webgpu_splats.js. Web-only — desktop builds use
// gl_raster/gl_rasterizer.cpp directly.
//
// Activation: append `?webgpu` to the URL or set
// `localStorage.CHESS_WEBGPU_SPLATS = "1"`. The init pass detects
// support at startup; if WebGPU isn't available (e.g. Linux Chrome
// without `chrome://flags/#enable-unsafe-webgpu`, Firefox stable,
// older Safari), the activation flag is honoured but harmless —
// init returns false, a small dismissible banner explains the
// fallback, and the chess web build renders splats via its existing
// per-quad WebGL path (which needs no flag anywhere).
//
// We never enable the software (forceFallbackAdapter) WebGPU path:
// SwiftShader running the 500k-splat compute pipelines is far
// slower than the per-quad WebGL renderer the page would otherwise
// use, so silently activating it would be a net downgrade.

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
//
// If the user opted in (`?webgpu` / localStorage) but WebGPU couldn't
// activate, the page falls back to the per-quad WebGL splat
// renderer and a brief dismissible banner explains why. Without
// opt-in, no banner appears — the page works exactly as it did
// before this file ever existed.
EM_JS(void, _chess_webgpu_init, (), {
  const url = new URL(window.location);
  const want = url.searchParams.has('webgpu') ||
               url.hash.includes('webgpu') ||
               (window.localStorage &&
                window.localStorage.getItem('CHESS_WEBGPU_SPLATS') === '1');
  if (!want) { Module._chess_webgpu_set_state(-1); return; }

  // ── Small dismissible "WebGPU unavailable" banner. Mounted on
  //    fallback so the user — who explicitly asked for WebGPU via
  //    `?webgpu` — sees a hint instead of silently getting the
  //    WebGL path. Auto-dismisses after 12s.
  const showFallbackBanner = (msg) => {
    if (document.getElementById('webgpu-fallback-banner')) return;
    const b = document.createElement('div');
    b.id = 'webgpu-fallback-banner';
    b.style.cssText =
        'position:fixed;top:8px;right:8px;z-index:1000;' +
        'max-width:340px;padding:10px 28px 10px 12px;' +
        'background:#222;color:#ddd;border:1px solid #444;' +
        'border-radius:6px;font:12px/1.4 monospace;' +
        'box-shadow:0 2px 8px rgba(0,0,0,0.5);';
    b.textContent = msg;
    const x = document.createElement('button');
    x.textContent = 'x';
    x.style.cssText =
        'position:absolute;top:4px;right:6px;width:18px;height:18px;' +
        'padding:0;border:0;background:transparent;color:#888;' +
        'cursor:pointer;font:14px monospace;';
    x.onclick = () => b.remove();
    b.appendChild(x);
    document.body.appendChild(b);
    setTimeout(() => { if (b.parentNode) b.remove(); }, 12000);
  };

  if (!window.WebGPUSplats || !window.WebGPUSplats.supported()) {
    console.info('[webgpu-splats] WebGPU not exposed by this ' +
                 'browser. Using the standard WebGL splat renderer.');
    showFallbackBanner(
        'WebGPU is not available in this browser. Rendering with ' +
        'the standard WebGL splat path. (Try a recent Chrome / ' +
        'Edge / Safari, or Firefox Nightly.)');
    Module._chess_webgpu_set_state(-1);
    return;
  }
  Module._chess_webgpu_set_state(1);  // initialising
  window.WebGPUSplats.init().then(ok => {
    if (ok) {
      console.log('[webgpu-splats] enabled');
      Module._chess_webgpu_set_state(2);
    } else {
      // WebGPUSplats.init() has already logged a clear info line
      // explaining which step bailed (no adapter / no context /
      // pipeline build). The banner echoes the most common cause
      // — Linux Chrome's chrome://flags/#enable-unsafe-webgpu gate
      // — without claiming it's definitely that.
      showFallbackBanner(
          'WebGPU could not activate. Rendering with the standard ' +
          'WebGL splat path. On Linux Chrome, enable ' +
          'chrome://flags/#enable-unsafe-webgpu and restart.');
      Module._chess_webgpu_set_state(-1);
    }
  }).catch(err => {
    console.info('[webgpu-splats] init error — falling back to ' +
                 'the standard WebGL splat renderer.', err);
    showFallbackBanner(
        'WebGPU init error: ' + (err && err.message ? err.message : err) +
        '. Rendering with the standard WebGL splat path.');
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
