#include "shatter_transition.h"

#include "board_renderer.h"
#include "shader.h"

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#else
#include <epoxy/gl.h>
#endif

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

GLuint g_shatter_program      = 0;
GLuint g_shatter_vao          = 0;
GLuint g_shatter_vbo          = 0;
int    g_shatter_vertex_count = 0;

GLuint g_capture_tex = 0;
// Single-sample FBO that the (possibly multisample) source framebuffer
// gets resolved into via glBlitFramebuffer. Needed on both platforms:
//   * Web: WebGL2's default backbuffer is created with antialias=true,
//     so it's multisampled and glCopyTexSubImage2D from it is
//     UNDEFINED (produces an all-zero texture in practice — black
//     shards).
//   * Desktop: the splat backdrop introduced an MS scene FBO and a
//     dedicated splat FBO. The chain of resolves leaves the
//     GtkGLArea-owned default FBO in a state where the safest read
//     is still an explicit glBlitFramebuffer into our own single-
//     sample texture-backed FBO; glCopyTexSubImage2D used to work
//     because the only target was the simple default FB, but post-
//     splat the implicit-source assumption stopped capturing what
//     we expected. Result was the same all-black shard texture.
GLuint g_capture_fbo = 0;
int g_capture_w = 0, g_capture_h = 0;

// Build shatter mesh: voronoi-like cells. For each jittered cell
// centre, compute its voronoi polygon via Sutherland-Hodgman
// clipping against each neighbour's perpendicular bisector, then
// fan-triangulate from the centroid.
// Vertex layout: centerNDC(2) localOffset(2) uv(2) seed(1) = 7 floats
struct SPoint { float x, y; };

std::vector<SPoint> clip_poly(const std::vector<SPoint>& poly,
                              float nx, float ny, float d) {
    // Inside half-plane: nx*p.x + ny*p.y + d <= 0
    std::vector<SPoint> out;
    if (poly.empty()) return out;
    auto side = [&](SPoint p) { return nx*p.x + ny*p.y + d; };
    int n = static_cast<int>(poly.size());
    for (int i = 0; i < n; i++) {
        SPoint a = poly[i];
        SPoint b = poly[(i + 1) % n];
        float da = side(a), db = side(b);
        if (da <= 0) {
            out.push_back(a);
            if (db > 0) {
                float t = da / (da - db);
                out.push_back({a.x + t*(b.x - a.x), a.y + t*(b.y - a.y)});
            }
        } else if (db <= 0) {
            float t = da / (da - db);
            out.push_back({a.x + t*(b.x - a.x), a.y + t*(b.y - a.y)});
        }
    }
    return out;
}

// Clip against the bisector between centre C and neighbour N (inside = closer to C).
std::vector<SPoint> clip_bisector(const std::vector<SPoint>& poly,
                                  SPoint c, SPoint n_pt) {
    SPoint mid = {(c.x + n_pt.x) * 0.5f, (c.y + n_pt.y) * 0.5f};
    float nx = n_pt.x - c.x, ny = n_pt.y - c.y;
    float d = -(nx*mid.x + ny*mid.y);
    return clip_poly(poly, nx, ny, d);
}

void build_shatter_mesh() {
    const int nx = 14, ny = 10;
    float cw = 2.0f / nx, ch = 2.0f / ny;

    auto hash2 = [](int ix, int iy) -> SPoint {
        float s1 = std::sin(ix * 127.1f + iy * 311.7f) * 43758.5453f;
        float s2 = std::sin(ix * 269.5f + iy * 183.3f) * 43758.5453f;
        return {s1 - std::floor(s1), s2 - std::floor(s2)};
    };
    auto hash1 = [](int col, int row) {
        float s = std::sin(col * 12.9898f + row * 78.233f) * 43758.5453f;
        return s - std::floor(s);
    };

    auto cell_center = [&](int col, int row) -> SPoint {
        SPoint j = hash2(col + 999, row + 1999);
        float jx = j.x * 0.7f + 0.15f;  // jitter within 15-85% of cell
        float jy = j.y * 0.7f + 0.15f;
        return {-1.0f + (col + jx) * cw, -1.0f + (row + jy) * ch};
    };

    std::vector<float> verts;

    for (int row = 0; row < ny; row++) {
        for (int col = 0; col < nx; col++) {
            SPoint center = cell_center(col, row);

            // Big initial polygon encompassing the screen.
            std::vector<SPoint> poly = {
                {center.x - 2.5f, center.y - 2.5f},
                {center.x + 2.5f, center.y - 2.5f},
                {center.x + 2.5f, center.y + 2.5f},
                {center.x - 2.5f, center.y + 2.5f},
            };

            // Clip against 24 neighbours in a 5x5 area for a better
            // voronoi approximation than the immediate 8-neighbourhood.
            for (int dr = -2; dr <= 2 && !poly.empty(); dr++) {
                for (int dc = -2; dc <= 2; dc++) {
                    if (dc == 0 && dr == 0) continue;
                    SPoint n_pt = cell_center(col + dc, row + dr);
                    poly = clip_bisector(poly, center, n_pt);
                    if (poly.empty()) break;
                }
            }

            // Clip against screen bounds [-1, 1].
            poly = clip_poly(poly,  1, 0, -1);
            poly = clip_poly(poly, -1, 0, -1);
            poly = clip_poly(poly,  0, 1, -1);
            poly = clip_poly(poly,  0,-1, -1);

            if (poly.size() < 3) continue;

            SPoint centroid = {0, 0};
            for (auto& p : poly) { centroid.x += p.x; centroid.y += p.y; }
            centroid.x /= poly.size();
            centroid.y /= poly.size();

            float seed = hash1(col, row);

            int np = static_cast<int>(poly.size());
            for (int i = 0; i < np; i++) {
                SPoint a = poly[i];
                SPoint b = poly[(i + 1) % np];

                auto add = [&](SPoint p) {
                    float lx = p.x - centroid.x, ly = p.y - centroid.y;
                    float u = (p.x + 1.0f) * 0.5f;
                    float v = (p.y + 1.0f) * 0.5f;
                    verts.insert(verts.end(),
                        {centroid.x, centroid.y, lx, ly, u, v, seed});
                };
                add(centroid);
                add(a);
                add(b);
            }
        }
    }

    g_shatter_vertex_count = static_cast<int>(verts.size() / 7);

    glGenVertexArrays(1, &g_shatter_vao);
    glGenBuffers(1, &g_shatter_vbo);
    glBindVertexArray(g_shatter_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_shatter_vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 7*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 7*sizeof(float), (void*)(2*sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 7*sizeof(float), (void*)(4*sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 7*sizeof(float), (void*)(6*sizeof(float)));
    glEnableVertexAttribArray(3);
    glBindVertexArray(0);
}

}  // namespace

void shatter_init() {
    g_shatter_program = create_program(shatter_vs_src, shatter_fs_src);
    build_shatter_mesh();
}

void renderer_capture_frame(int width, int height) {
    // Snapshot whichever framebuffer is currently bound — that's the
    // one app_state.cpp's render_board just finished drawing into.
    // On desktop it's the GtkGLArea's internal FBO; on web it's FBO 0
    // (the WebGL canvas). Either way we want to resolve it into our
    // own single-sample texture-backed FBO so glBlitFramebuffer
    // handles multisample auto-resolve and the captured pixels match
    // what the user actually saw.
    GLint src_fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &src_fb);

    if (g_capture_tex == 0 || g_capture_w != width || g_capture_h != height) {
        if (g_capture_tex) glDeleteTextures(1, &g_capture_tex);
        glGenTextures(1, &g_capture_tex);
        glBindTexture(GL_TEXTURE_2D, g_capture_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g_capture_w = width;
        g_capture_h = height;

        if (!g_capture_fbo) glGenFramebuffers(1, &g_capture_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, g_capture_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, g_capture_tex, 0);
        // Re-bind whatever was current so we don't yank GTK / the
        // emscripten canvas out from under the next draw.
        glBindFramebuffer(GL_FRAMEBUFFER, src_fb);
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_capture_fbo);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);

#ifdef __EMSCRIPTEN__
    // When `?webgpu` is active the per-quad WebGL splat blit is
    // skipped during the live frame so the WebGPU canvas behind can
    // show through — which means the FBO 0 we just captured has
    // chess pieces over an alpha-cleared transparent region instead
    // of a splat backdrop. Re-composite the WebGL splat colour
    // texture *under* the captured chess content so the shatter
    // shards show a full scene instead of dark voids. Cheap: one
    // fullscreen quad. No-op when no splat data is loaded or the
    // per-quad splat cache isn't valid (e.g. challenge-summary
    // screen where renderer_draw didn't run this frame).
    renderer_composite_splat_under(g_capture_fbo, width, height);
#endif

    // One-shot diagnostic: read a single pixel from the centre of
    // the capture and print its RGBA. If this prints (0,0,0,*) the
    // capture isn't reading what we expected; if it prints non-zero
    // RGB the capture is fine and the bug is downstream (shader,
    // bindings, blend). Logged once so the console doesn't drown.
    static bool s_diag_done = false;
    if (!s_diag_done) {
        s_diag_done = true;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_capture_fbo);
        unsigned char px[4] = {0, 0, 0, 0};
        glReadPixels(width / 2, height / 2, 1, 1,
                     GL_RGBA, GL_UNSIGNED_BYTE, px);
        std::fprintf(stderr,
            "[shatter] capture diag: center pixel RGBA = "
            "(%u, %u, %u, %u), size=%dx%d, src_fb=%d\n",
            px[0], px[1], px[2], px[3], width, height,
            static_cast<int>(src_fb));
    }

    // Restore the original FBO binding for both read and draw so
    // subsequent draws (the new puzzle render inside the trigger)
    // land where the caller expects.
    glBindFramebuffer(GL_FRAMEBUFFER, src_fb);
}

void renderer_draw_shatter(float t, int width, int height) {
    if (g_capture_tex == 0) return;
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(g_shatter_program);
    glUniform1f(glGetUniformLocation(g_shatter_program, "uTime"), t);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_capture_tex);
    glUniform1i(glGetUniformLocation(g_shatter_program, "uTex"), 0);

    glBindVertexArray(g_shatter_vao);
    glDrawArrays(GL_TRIANGLES, 0, g_shatter_vertex_count);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}
