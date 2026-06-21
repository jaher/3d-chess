#pragma once

// Cross-vendor compute-shader port of the CUDA tile-based gsplat
// rasterizer (see native/cuda_raster/). Same algorithm — preprocess
// → tile-bin → per-tile depth sort → per-pixel front-to-back
// composite — but implemented in GLSL compute shaders so it runs on
// AMD / Intel / Apple GPUs without a CUDA toolchain.
//
// Drop-in API match for cuda_raster::CudaRasterizer so the renderer
// can swap them via env var (MARBLE_CUDA_RASTERIZER=1 vs
// MARBLE_GL_COMPUTE_RASTERIZER=1).

#include <epoxy/gl.h>

#include "../splat.h"

#include <memory>
#include <vector>

namespace gl_raster {

class GlRasterizer {
public:
    GlRasterizer();
    ~GlRasterizer();

    GlRasterizer(const GlRasterizer&) = delete;
    GlRasterizer& operator=(const GlRasterizer&) = delete;

    // Upload splats once at viewer startup. The rasterizer copies into
    // SSBOs and never touches the host array again.
    //
    // `flip_y_in_data` pre-flips the splat positions + adjusts each
    // splat's local rotation so the host can apply a Y-flip via the
    // model matrix `S(s, -s, s)` without breaking the rasterizer's
    // rotation-only Shoemake decomposition (which can't handle
    // reflections). When set, the caller should pass a model matrix
    // with POSITIVE scale on Y; the actual Y-flip is baked into the
    // data here. Used by 3d_chess where Marble's coordinate system
    // is flipped relative to the chess world.
    void upload(const std::vector<Splat>& splats,
                bool flip_y_in_data = false);

    // Resize the output target. Caller is responsible for binding
    // an `image2D` GL_RGBA8 at the dims given. We use this to pick
    // the tile grid + SSBO scratch sizes.
    void resize(int w, int h);

    // Bind the GL_RGBA8 texture the rasterizer should write into.
    // Use the HiDPI FBO's color attachment.
    void set_output_texture(GLuint tex);

    // Optional GL_R32F texture receiving the per-pixel splat-cloud
    // "surface" distance (view-space) for depth-correct compositing.
    // 0 (default) disables the depth output. See out_depth in
    // kGlslRasterize.
    void set_depth_output_texture(GLuint tex);

    // Render one frame. `view`, `proj`, `model` are column-major 4x4
    // matrices (same layout as Mat4::m).
    void render(const float* view, const float* proj, const float* model);

    int num_splats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gl_raster
