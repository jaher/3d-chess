# Splat World — phone-to-glasses Gaussian-splat streaming

Take a photo on your phone → [World Labs Marble](https://www.worldlabs.ai/)
generates a full 3D Gaussian-splat world from it → the server renders that
world on the CPU and **streams JPEG frames to a Meta Ray-Ban Display**, where
the Neural Band walks the camera around. The glasses do *zero* 3D rendering —
they only display a stream and forward input, which is exactly what a small
monocular display with a limited browser is good at.

```
 phone (capture page) ──photo (base64)──▶ Node server ──worlds:generate──▶ Marble API
                                              │  ◀───── .spz splat file ──────┘
                                              ▼
                                        splat_render (C++)
                                        CPU EWA rasterizer
                                              │  JPEG frames
                                              ▼
 glasses (viewer page) ◀──WebSocket frames────┤
        │                                     │
        └──Neural Band nav (keydown→WS)──────▶┘  server owns the camera pose
```

## Components

| File | Role |
| --- | --- |
| `server.js` | Dependency-free Node server: static pages, Marble API proxy (`$WORLD_LABS_API_KEY` stays server-side), `.spz` download cache, renderer child management, WebSocket + MJPEG frame fan-out. |
| `renderer.cpp` → `splat_render` | Standalone CPU Gaussian-splat rasterizer: EWA splatting (Zwicker 2001) with front-to-back tile rasterization and per-pixel early termination (Kerbl 2023, on the CPU, OpenMP over 32×32 tiles). Reuses the repo's proven SPZ decoder (`../splat.cpp`) and gunzip (`../compression.cpp`) unmodified. Persistent child process: pose lines in on stdin, `F <n>\n` + JPEG out on stdout. |
| `phone.html` | Phone page: take/choose a photo (downscaled on-device, sent as `data_base64` — no public hosting needed), optional text prompt, generate + poll, pick any existing Marble world, live preview, debug nav buttons. |
| `glasses.html` | Ray-Ban Display Web App page (600×600, `mrbd-web-app-capable`, black = transparent on the additive lens): swaps streamed JPEG frames into an `<img>`, forwards Neural Band input. Falls back to MJPEG if the WebSocket drops. |
| `smoke_test.sh` | Offline end-to-end test: renderer JPEG framing, server boot, nav changes the frame, MJPEG stream. `make test`. |

## Run

```bash
make -C splat_glasses                      # build splat_render (needs g++, zlib)
export WORLD_LABS_API_KEY=...              # only needed for generation/listing
node splat_glasses/server.js 8095
```

- Phone: `http://<server>:8095/` — capture a photo, generate, send to glasses.
- Glasses: `http://<server>:8095/glasses` — installs as a Web App via
  `manifest.json`. Real glasses require HTTPS/WSS, so front this with a TLS
  reverse proxy (same constraint as `../glasses/`); plain HTTP works for
  desktop-browser testing.
- On boot the server auto-loads `../world_labs/datacenter/splat_500k.spz`
  (if present) so the viewer works before any generation.

## Navigation (Neural Band)

The Neural Band and frame cap-touch surface to the glasses browser as
ordinary `keydown` events (same input model as the chess glasses app):

| Gesture / key | Action |
| --- | --- |
| swipe left / right (◀ ▶) | turn 12° |
| swipe up / down (▲ ▼) | walk forward / back 0.35 m |
| pinch (Enter) | reset view to the world origin |

The server owns the camera pose; nav events go glasses → server over the
WebSocket, the server re-renders, and the new frame streams back. Latest-wins
pacing: while a frame is in flight new poses just replace the pending one, so
the stream never lags behind the band.

## Measured (24-core desktop, 500k-splat Marble world, 480×480)

- render ≈ 12–14 ms + JPEG encode ≈ 3 ms per frame
- sustained end-to-end over WebSocket: **~40 fps** at ~31 KB/frame (~1.3 MB/s)
- world hot-swap (CDN download 8 MB + load + first frame): a few seconds

## Honest v1 notes

- Neural Band input is *presumed* to arrive as arrow/Enter `keydown` events —
  that is how the shipped chess glasses app receives it, but this app has
  been verified with a desktop browser + keyboard, not on physical glasses.
- Generation costs Marble credits and takes minutes; the worlds list lets you
  reuse anything already generated.
- The renderer drops spherical harmonics (SH0 only, like the SPZ tier files
  themselves) and renders flat RGB — matches what the desktop viewer shows.
- One world + one camera per server (all viewers see the same stream).
