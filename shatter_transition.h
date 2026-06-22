#pragma once

// Glass-shatter post-process used for challenge puzzle transitions.
// renderer_capture_frame snapshots the current backbuffer; on
// subsequent frames renderer_draw_shatter animates the captured
// texture as a voronoi mesh of shards falling away.

// One-time setup: compile the shatter shader and build the voronoi
// shard mesh. Call once from the renderer's GL-init path.
void shatter_init();

// Copy the default framebuffer's colour attachment into the capture
// texture so renderer_draw_shatter can animate it afterwards.
void renderer_capture_frame(int width, int height);

// Ensure the capture FBO + texture + depth attachment exist at the
// given size and return the FBO handle. Use it when the caller wants
// to render directly into the capture target instead of relying on
// renderer_capture_frame to read from the default FB — on web that
// read path is unreliable because the WebGL2 multisample default
// framebuffer doesn't always resolve via glBlitFramebuffer the way
// the spec implies, and re-rendering the live state straight into
// our own single-sample FBO sidesteps the quirk entirely.
unsigned int shatter_ensure_capture_target(int width, int height);

// Draw the shattered captured frame at animation time `t` (seconds).
// No-op before the first capture.
void renderer_draw_shatter(float t, int width, int height);
