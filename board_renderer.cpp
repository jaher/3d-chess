#include "board_renderer.h"
#include "chess_rules.h"
#include "mat.h"
#include "render_internal.h"
#include "shader.h"
#include "shatter_transition.h"
#include "text_atlas.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#ifndef __EMSCRIPTEN__
#include <epoxy/gl.h>
#include <glib.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#else
#include <GLES3/gl3.h>
#include <emscripten.h>
// Provided by web/font_atlas_stb.cpp; bakes the same 16x6 cell atlas the
// desktop Cairo path produces, and binds it to *out_tex with GL_R8.
extern "C" void build_font_atlas_stb(unsigned int* out_tex,
                                     int atlas_w, int atlas_h);
// glib monotonic time replacement returning microseconds since process start.
typedef int64_t gint64;
static inline gint64 g_get_monotonic_time() {
    return static_cast<gint64>(emscripten_get_now() * 1000.0);
}
#endif

// ---------------------------------------------------------------------------
// GL state
// ---------------------------------------------------------------------------
static PieceGPU g_pieces[PIECE_COUNT];
static GLuint g_board_vao = 0, g_board_vbo = 0;
static int g_board_light_count = 0, g_board_dark_count = 0;
// Shared between board_renderer.cpp and the per-screen render
// modules (challenge_ui.cpp, …) via render_internal.h. Intentionally
// non-static so those TUs can link against them; no other module
// should touch these symbols.
GLuint g_text_program = 0;
GLuint g_highlight_program = 0;
GLuint g_font_tex = 0;

static GLuint g_program = 0;
static GLuint g_shadow_program = 0;
static GLuint g_shadow_fbo = 0, g_shadow_tex = 0;
static constexpr int SHADOW_MAP_SIZE = 4096;
static GLuint g_disc_vao = 0, g_disc_vbo = 0;
static int g_disc_vertex_count = 0;
static GLuint g_ring_vao = 0, g_ring_vbo = 0;

// Font rendering (g_text_program / g_font_tex are exported via
// render_internal.h for the per-screen modules).
static GLuint g_label_vao = 0, g_label_vbo = 0;
static int g_label_vertex_count = 0;
static int g_ring_vertex_count = 0;

// Shatter transition state (program, shard mesh, capture texture) now
// lives in shatter_transition.{h,cpp}; renderer_init kicks its setup
// via shatter_init().

// Menu-physics collision geometry (per-piece half-extents + slice
// sub-boxes) has moved to menu_physics.{h,cpp}. renderer_init just
// hands the loaded STLs to menu_physics_init and doesn't see the
// collision tables directly.

// Cartoon-outline post-process. Scene FBO has color + depth
// textures, both sampleable from g_outline_program. Size tracks
// the current window, recreated lazily on resize.
static GLuint g_outline_program  = 0;
static GLuint g_scene_fbo        = 0;
static GLuint g_scene_color_tex  = 0;
static GLuint g_scene_depth_tex  = 0;
static int    g_scene_fbo_w      = 0;
static int    g_scene_fbo_h      = 0;
// Two-triangle NDC fullscreen quad, reused by the outline pass.
static GLuint g_fullscreen_vao   = 0;
static GLuint g_fullscreen_vbo   = 0;

// Lazily (re)allocate the scene FBO textures for a new window size.
// Creates the FBO on first call. Cheap (six GL calls) — safe to
// call every frame when the outline is on.
static void ensure_scene_fbo(int w, int h) {
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    if (g_scene_fbo && g_scene_fbo_w == w && g_scene_fbo_h == h) return;

    if (g_scene_fbo == 0)       glGenFramebuffers(1, &g_scene_fbo);
    if (g_scene_color_tex == 0) glGenTextures(1, &g_scene_color_tex);
    if (g_scene_depth_tex == 0) glGenTextures(1, &g_scene_depth_tex);

    glBindTexture(GL_TEXTURE_2D, g_scene_color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, g_scene_depth_tex);
    // 24-bit depth; matches the shadow-map format already used
    // elsewhere in this file and is sampleable on both desktop
    // and WebGL 2 / GLES 3.0.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
                 GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindFramebuffer(GL_FRAMEBUFFER, g_scene_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g_scene_color_tex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, g_scene_depth_tex, 0);

    g_scene_fbo_w = w;
    g_scene_fbo_h = h;
}

// Run the cartoon-outline post-process. The caller is expected to
// have just drawn the 3D scene into g_scene_fbo; this binds the
// default framebuffer, samples the scene's colour + depth textures,
// and writes the darkened-edge result. Leaves depth testing re-enabled
// so subsequent UI passes behave the same as in the no-outline path.
static void run_outline_post_process(GLuint default_fbo, int width, int height) {
    glBindFramebuffer(GL_FRAMEBUFFER, default_fbo);
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(g_outline_program);
    glUniform1i(glGetUniformLocation(g_outline_program, "uColorTex"), 0);
    glUniform1i(glGetUniformLocation(g_outline_program, "uDepthTex"), 1);
    glUniform2f(glGetUniformLocation(g_outline_program, "uTexelSize"),
                1.0f / static_cast<float>(width),
                1.0f / static_cast<float>(height));
    // Must match the near/far plane of the perspective matrix used to
    // render into the scene FBO — the shader uses them to linearise
    // depth so edge strength is distance-invariant.
    glUniform1f(glGetUniformLocation(g_outline_program, "uNear"), 0.1f);
    glUniform1f(glGetUniformLocation(g_outline_program, "uFar"),  100.0f);
    glUniform1f(glGetUniformLocation(g_outline_program, "uEdgeStrength"), 4.0f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_scene_color_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_scene_depth_tex);

    glBindVertexArray(g_fullscreen_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // Leave texture unit 0 active for subsequent draws.
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_DEPTH_TEST);
}

// ---------------------------------------------------------------------------
// Mesh builders
// ---------------------------------------------------------------------------
static void build_disc_mesh(float radius, int segments,
                            GLuint& vao, GLuint& vbo, int& vert_count) {
    std::vector<float> verts;
    float step = 2.0f * static_cast<float>(M_PI) / segments;
    for (int i = 0; i < segments; i++) {
        float a0 = step * i, a1 = step * (i + 1);
        verts.insert(verts.end(), {0.0f, 0.0f, 0.0f});
        verts.insert(verts.end(), {std::cos(a0)*radius, 0.0f, std::sin(a0)*radius});
        verts.insert(verts.end(), {std::cos(a1)*radius, 0.0f, std::sin(a1)*radius});
    }
    vert_count = static_cast<int>(verts.size() / 3);
    glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)), verts.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

static void build_ring_mesh(float r_inner, float r_outer, int segments,
                            GLuint& vao, GLuint& vbo, int& vert_count) {
    std::vector<float> verts;
    float step = 2.0f * static_cast<float>(M_PI) / segments;
    for (int i = 0; i < segments; i++) {
        float a0 = step * i, a1 = step * (i + 1);
        float c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1), s1 = std::sin(a1);
        verts.insert(verts.end(), {c0*r_inner, 0, s0*r_inner});
        verts.insert(verts.end(), {c0*r_outer, 0, s0*r_outer});
        verts.insert(verts.end(), {c1*r_outer, 0, s1*r_outer});
        verts.insert(verts.end(), {c0*r_inner, 0, s0*r_inner});
        verts.insert(verts.end(), {c1*r_outer, 0, s1*r_outer});
        verts.insert(verts.end(), {c1*r_inner, 0, s1*r_inner});
    }
    vert_count = static_cast<int>(verts.size() / 3);
    glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)), verts.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

static std::vector<float> build_board_mesh(int& light_verts, int& dark_verts) {
    std::vector<float> light, dark;
    float y = BOARD_Y;
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            float cx, cz; square_center(col, row, cx, cz);
            float h = SQ / 2.0f;
            float x0 = cx - h, x1 = cx + h, z0 = cz - h, z1 = cz + h;
            // Standard chess square colors: a1 is dark (both player's
            // left-hand corner is a dark square). Internal col 7 = a-file,
            // so a1 is (row=0, col=7) where (row+col)%2=1; we want that in
            // the dark buffer, and squares where (row+col)%2=0 are light.
            // This also puts queens on their own color: d1 (row=0, col=4)
            // is light, d8 (row=7, col=4) is dark.
            auto& buf = ((row + col) % 2 == 0) ? light : dark;
            buf.insert(buf.end(), {0,1,0, x0,y,z0}); buf.insert(buf.end(), {0,1,0, x1,y,z0});
            buf.insert(buf.end(), {0,1,0, x1,y,z1}); buf.insert(buf.end(), {0,1,0, x0,y,z0});
            buf.insert(buf.end(), {0,1,0, x1,y,z1}); buf.insert(buf.end(), {0,1,0, x0,y,z1});
        }
    }
    float thickness = 0.45f, bmin = -4.0f * SQ, bmax = 4.0f * SQ, ybot = y - thickness;
    // Bottom
    light.insert(light.end(), {0,-1,0, bmin,ybot,bmin}); light.insert(light.end(), {0,-1,0, bmax,ybot,bmin});
    light.insert(light.end(), {0,-1,0, bmax,ybot,bmax}); light.insert(light.end(), {0,-1,0, bmin,ybot,bmin});
    light.insert(light.end(), {0,-1,0, bmax,ybot,bmax}); light.insert(light.end(), {0,-1,0, bmin,ybot,bmax});
    // 4 sides
    auto side = [&](float nx, float ny, float nz, float x0, float z0, float x1, float z1) {
        dark.insert(dark.end(), {nx,ny,nz, x0,ybot,z0}); dark.insert(dark.end(), {nx,ny,nz, x1,ybot,z1});
        dark.insert(dark.end(), {nx,ny,nz, x1,y,z1});    dark.insert(dark.end(), {nx,ny,nz, x0,ybot,z0});
        dark.insert(dark.end(), {nx,ny,nz, x1,y,z1});    dark.insert(dark.end(), {nx,ny,nz, x0,y,z0});
    };
    side(0,0,1, bmin,bmax, bmax,bmax); side(0,0,-1, bmax,bmin, bmin,bmin);
    side(1,0,0, bmax,bmin, bmax,bmax); side(-1,0,0, bmin,bmax, bmin,bmin);

    light_verts = static_cast<int>(light.size() / 6);
    dark_verts = static_cast<int>(dark.size() / 6);
    std::vector<float> all;
    all.insert(all.end(), light.begin(), light.end());
    all.insert(all.end(), dark.begin(), dark.end());
    return all;
}

static void upload_piece(PieceGPU& gpu, const StlModel& model) {
    // Smooth per-vertex normals are only worth computing on decimated
    // meshes — at web build density (~80k tris) raw face normals show
    // visible facets. Hi-res desktop meshes (~1M tris each) have sub-
    // pixel triangles where flat shading is indistinguishable from
    // smooth, and the hash-map dance in build_vertex_buffer costs
    // seconds at that size. Flip to the flat fast-path above the
    // threshold.
    constexpr size_t SMOOTH_TRI_LIMIT = 200'000;
    float crease = model.triangle_count() > SMOOTH_TRI_LIMIT ? 0.0f : 60.0f;
    std::vector<float> buf = model.build_vertex_buffer(crease);
    gpu.num_vertices = static_cast<int>(buf.size() / 6);
    glGenVertexArrays(1, &gpu.vao); glGenBuffers(1, &gpu.vbo);
    glBindVertexArray(gpu.vao);
    glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(buf.size() * sizeof(float)), buf.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// Draw helpers
// ---------------------------------------------------------------------------
static void set_material(GLuint prog, float r, float g, float b,
                         float metallic, float roughness, float ao, int wood) {
    glUniform3f(glGetUniformLocation(prog, "uAlbedo"), r, g, b);
    glUniform1f(glGetUniformLocation(prog, "uMetallic"), metallic);
    glUniform1f(glGetUniformLocation(prog, "uRoughness"), roughness);
    glUniform1f(glGetUniformLocation(prog, "uAO"), ao);
    glUniform1i(glGetUniformLocation(prog, "uWoodEffect"), wood);
}

static void draw_with_model(GLuint prog, const Mat4& model_mat, GLuint vao, int count) {
    float nm[9]; mat4_normal_matrix(model_mat, nm);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uModel"), 1, GL_FALSE, model_mat.m);
    glUniformMatrix3fv(glGetUniformLocation(prog, "uNormalMat"), 1, GL_FALSE, nm);
    glBindVertexArray(vao); glDrawArrays(GL_TRIANGLES, 0, count); glBindVertexArray(0);
}

// Piece model matrix
static Mat4 piece_model_matrix(float wx, float wz, float s, bool is_white, float rot_z_to_y) {
    Mat4 orient = mat4_rotate_x(rot_z_to_y);
    if (!is_white)
        orient = mat4_multiply(mat4_rotate_y(static_cast<float>(M_PI)), orient);
    return mat4_multiply(mat4_translate(wx, BOARD_Y + s, wz), mat4_multiply(mat4_scale(s, s, s), orient));
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
// Build textured quads for all board labels
// Each vertex: x, y, z, u, v (5 floats)
static void build_label_mesh() {
    std::vector<float> verts;

    float label_y = BOARD_Y + 0.003f;
    float char_w = 0.28f, char_h = 0.35f;
    float margin = 0.38f;

    // Direction vectors (readable from white's side, rot_y=180)
    // screen-right = world -X, screen-up = world +Z
    float rx = -1, rz = 0;
    float ux = 0,  uz = 1;

    auto add_quad = [&](float cx, float cz, char ch) {
        float u0, v0, u1, v1;
        char_uvs(ch, u0, v0, u1, v1);
        float hw = char_w * 0.5f, hh = char_h * 0.5f;

        float lx[4] = {-hw,  hw,  hw, -hw};
        float ly[4] = {-hh, -hh,  hh,  hh};
        float wu[4] = {u0, u1, u1, u0};
        float wv[4] = {v1, v1, v0, v0};

        float wx[4], wz[4];
        for (int i = 0; i < 4; i++) {
            wx[i] = cx + lx[i] * rx + ly[i] * ux;
            wz[i] = cz + lx[i] * rz + ly[i] * uz;
        }

        int idx[6] = {0,1,2, 0,2,3};
        for (int i = 0; i < 6; i++) {
            int j = idx[i];
            verts.insert(verts.end(), {wx[j], label_y, wz[j], wu[j], wv[j]});
        }
    };

    // File letters (a-h) along both edges
    for (int col = 0; col < 8; col++) {
        float cx, cz_unused;
        square_center(col, 0, cx, cz_unused);
        char letter = 'a' + (7 - col); // screen-left (+X, col 7) = 'a', screen-right (-X, col 0) = 'h'
        add_quad(cx, -4.0f * SQ - margin, letter);
        add_quad(cx,  4.0f * SQ + margin, letter);
    }

    // Rank numbers (1-8) along both edges
    for (int row = 0; row < 8; row++) {
        float cx_unused, cz;
        square_center(0, row, cx_unused, cz);
        char digit = '1' + row;
        add_quad( 4.0f * SQ + margin, cz, digit);
        add_quad(-4.0f * SQ - margin, cz, digit);
    }

    g_label_vertex_count = static_cast<int>(verts.size() / 5);

    glGenVertexArrays(1, &g_label_vao);
    glGenBuffers(1, &g_label_vbo);
    glBindVertexArray(g_label_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_label_vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)), verts.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
}

void renderer_init(StlModel loaded_models[PIECE_COUNT]) {
    g_program = create_program(vertex_shader_src, fragment_shader_src);
    g_highlight_program = create_program(highlight_vs_src, highlight_fs_src);
    g_shadow_program = create_program(shadow_vs_src, shadow_fs_src);
    g_text_program = create_program(text_vs_src, text_fs_src);
    g_outline_program = create_program(outline_vs_src, outline_fs_src);
    shatter_init();

    // Menu-piece collision geometry: rotated-AABB extents + the
    // per-slice sub-box stack used for piece-piece contact.
    menu_physics_init(loaded_models);

    // Two-triangle fullscreen quad in clip space for the outline
    // post-process. Position-only attribute at location 0 matches
    // the outline_vs layout. One VAO/VBO for the lifetime of the
    // program.
    {
        const float quad_verts[] = {
            -1.0f, -1.0f,
             1.0f, -1.0f,
             1.0f,  1.0f,
            -1.0f, -1.0f,
             1.0f,  1.0f,
            -1.0f,  1.0f,
        };
        glGenVertexArrays(1, &g_fullscreen_vao);
        glGenBuffers(1, &g_fullscreen_vbo);
        glBindVertexArray(g_fullscreen_vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_fullscreen_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad_verts), quad_verts,
                     GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                              2 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);
    }

    // Shadow map
    glGenFramebuffers(1, &g_shadow_fbo); glGenTextures(1, &g_shadow_tex);
    glBindTexture(GL_TEXTURE_2D, g_shadow_tex);
#ifdef __EMSCRIPTEN__
    // WebGL 2 requires GL_UNSIGNED_INT (or GL_UNSIGNED_INT_24_8 with stencil)
    // for GL_DEPTH_COMPONENT24; GL_FLOAT only pairs with GL_DEPTH_COMPONENT32F.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
#else
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
#endif
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
#ifdef __EMSCRIPTEN__
    // WebGL 2 lacks GL_CLAMP_TO_BORDER and per-texture border colors; use
    // CLAMP_TO_EDGE. The PBR fragment shader bounds-checks projCoords.xy
    // against [0,1] before sampling, so the missing white border doesn't
    // produce false shadows.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#else
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float bc[] = {1,1,1,1}; glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, bc);
#endif
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, g_shadow_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_shadow_tex, 0);
#ifdef __EMSCRIPTEN__
    // WebGL 2 has no glDrawBuffer (singular); use glDrawBuffers with NONE
    // to indicate no color attachments for this depth-only FBO.
    {
        GLenum none_bufs[] = { GL_NONE };
        glDrawBuffers(1, none_bufs);
    }
    glReadBuffer(GL_NONE);
#else
    glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE);
#endif
    {
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE)
            std::fprintf(stderr, "Shadow FBO incomplete: 0x%x\n", status);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    for (int i = 0; i < PIECE_COUNT; i++)
        upload_piece(g_pieces[i], loaded_models[i]);

    int lv, dv;
    auto board_buf = build_board_mesh(lv, dv);
    g_board_light_count = lv; g_board_dark_count = dv;
    glGenVertexArrays(1, &g_board_vao); glGenBuffers(1, &g_board_vbo);
    glBindVertexArray(g_board_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_board_vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(board_buf.size() * sizeof(float)), board_buf.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    build_disc_mesh(0.48f, 48, g_disc_vao, g_disc_vbo, g_disc_vertex_count);
    build_ring_mesh(0.38f, 0.48f, 48, g_ring_vao, g_ring_vbo, g_ring_vertex_count);

    glEnable(GL_DEPTH_TEST);
    // Font atlas and board labels
    build_font_atlas();
    build_label_mesh();

    glClearColor(0.12f, 0.12f, 0.15f, 1.0f);
}

// ---------------------------------------------------------------------------
// Main draw
// ---------------------------------------------------------------------------
// NDC rectangle for the "Back to Menu" button that appears inside
// the game-over overlay in renderer_draw. Shared between the draw
// path and endgame_menu_button_hit_test below so they stay in sync.
// Width picked to leave ~0.03 NDC of padding on each side of the
// "Back to Menu" label (which is ~0.235 wide at the current font).
static const float EG_MENU_BTN_X = -0.15f;
static const float EG_MENU_BTN_Y = -0.015f;
static const float EG_MENU_BTN_W =  0.30f;
static const float EG_MENU_BTN_H =  0.07f;

// In analysis mode we additionally show a "Continue Playing" button
// stacked above the Back to Menu button. Slightly wider because the
// label is ~0.30 NDC at the button font.
static const float EG_CONT_BTN_X = -0.18f;
static const float EG_CONT_BTN_Y =  0.072f;
static const float EG_CONT_BTN_W =  0.36f;
static const float EG_CONT_BTN_H =  0.07f;

bool endgame_menu_button_hit_test(double mx, double my, int width, int height) {
    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float ndc_y = 1.0f - 2.0f * static_cast<float>(my) / height;
    return ndc_x >= EG_MENU_BTN_X && ndc_x <= EG_MENU_BTN_X + EG_MENU_BTN_W &&
           ndc_y >= EG_MENU_BTN_Y - EG_MENU_BTN_H && ndc_y <= EG_MENU_BTN_Y;
}

bool analysis_continue_button_hit_test(double mx, double my,
                                       int width, int height) {
    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float ndc_y = 1.0f - 2.0f * static_cast<float>(my) / height;
    return ndc_x >= EG_CONT_BTN_X && ndc_x <= EG_CONT_BTN_X + EG_CONT_BTN_W &&
           ndc_y >= EG_CONT_BTN_Y - EG_CONT_BTN_H && ndc_y <= EG_CONT_BTN_Y;
}

// ---------------------------------------------------------------------------
// Withdraw confirmation modal — dialog box centred on screen with
// Yes / No buttons. Constants shared between the draw path and the
// hit-test so they stay in sync.
// ---------------------------------------------------------------------------
// Dialog panel (outer outline is drawn slightly larger).
static const float WC_PANEL_X0 = -0.30f;
static const float WC_PANEL_X1 =  0.30f;
static const float WC_PANEL_Y0 = -0.14f;
static const float WC_PANEL_Y1 =  0.18f;

// Yes button (green).
static const float WC_YES_X0 = -0.22f;
static const float WC_YES_X1 = -0.02f;
static const float WC_YES_Y0 = -0.10f;
static const float WC_YES_Y1 =  0.00f;

// No button (red).
static const float WC_NO_X0 =  0.02f;
static const float WC_NO_X1 =  0.22f;
static const float WC_NO_Y0 = -0.10f;
static const float WC_NO_Y1 =  0.00f;

// "Pieces missing" modal (CMM = Chessnut Missing-pieces Modal).
// Wider/taller than the withdraw modal so it can fit a title, a
// missing-squares list, a hint, and a single Exit-to-Menu button.
static const float CMM_PANEL_X0 = -0.42f;
static const float CMM_PANEL_X1 =  0.42f;
static const float CMM_PANEL_Y0 = -0.22f;
static const float CMM_PANEL_Y1 =  0.26f;

// Single Exit-to-Menu button, centred at the bottom of the panel.
static const float CMM_EXIT_X0 = -0.18f;
static const float CMM_EXIT_X1 =  0.18f;
static const float CMM_EXIT_Y0 = -0.16f;
static const float CMM_EXIT_Y1 = -0.06f;

bool chessnut_missing_exit_button_hit_test(double mx, double my,
                                           int width, int height) {
    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float ndc_y = 1.0f - 2.0f * static_cast<float>(my) / height;
    return ndc_x >= CMM_EXIT_X0 && ndc_x <= CMM_EXIT_X1 &&
           ndc_y >= CMM_EXIT_Y0 && ndc_y <= CMM_EXIT_Y1;
}

bool withdraw_confirm_hit_test(double mx, double my,
                               int width, int height, int* which) {
    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float ndc_y = 1.0f - 2.0f * static_cast<float>(my) / height;
    if (which) *which = 0;
    if (ndc_x >= WC_YES_X0 && ndc_x <= WC_YES_X1 &&
        ndc_y >= WC_YES_Y0 && ndc_y <= WC_YES_Y1) {
        if (which) *which = 1;
        return true;
    }
    if (ndc_x >= WC_NO_X0 && ndc_x <= WC_NO_X1 &&
        ndc_y >= WC_NO_Y0 && ndc_y <= WC_NO_Y1) {
        if (which) *which = 2;
        return true;
    }
    // Clicks anywhere inside the panel are swallowed by the modal but
    // don't map to either button — return true with *which == 0 so
    // the dispatcher knows not to pass the click through to the board.
    if (ndc_x >= WC_PANEL_X0 && ndc_x <= WC_PANEL_X1 &&
        ndc_y >= WC_PANEL_Y0 && ndc_y <= WC_PANEL_Y1) {
        return true;
    }
    // Click outside the panel — also swallowed (modal semantics).
    return true;
}

bool flag_hit_test(const ClothFlag& flag,
                   double mx, double my, int width, int height) {
    if (flag.p.empty()) return false;
    float x0, y0, x1, y1;
    flag_bbox(flag, x0, y0, x1, y1);
    // Small NDC padding so the rippling trailing edge doesn't make
    // the click area frustrating to hit.
    const float pad = 0.015f;
    x0 -= pad; y0 -= pad; x1 += pad; y1 += pad;
    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float ndc_y = 1.0f - 2.0f * static_cast<float>(my) / height;
    return ndc_x >= x0 && ndc_x <= x1 && ndc_y >= y0 && ndc_y <= y1;
}

// ---------------------------------------------------------------------------
// Small helper: push six vertices (two triangles) for an axis-aligned
// NDC quad into a flat float buffer. Each vertex is xyz (z=0).
// ---------------------------------------------------------------------------
static void push_quad(std::vector<float>& verts,
                      float x0, float y0, float x1, float y1) {
    verts.push_back(x0); verts.push_back(y0); verts.push_back(0);
    verts.push_back(x1); verts.push_back(y0); verts.push_back(0);
    verts.push_back(x1); verts.push_back(y1); verts.push_back(0);
    verts.push_back(x0); verts.push_back(y0); verts.push_back(0);
    verts.push_back(x1); verts.push_back(y1); verts.push_back(0);
    verts.push_back(x0); verts.push_back(y1); verts.push_back(0);
}

// ---------------------------------------------------------------------------
// Format a millisecond budget as the display string for the in-game
// clock widget: "M:SS" for ≥10 s remaining, "S.T" (seconds + tenths)
// for the last ten seconds. This is the canonical convention every
// online chess UI uses.
// ---------------------------------------------------------------------------
static std::string format_clock_ms(int64_t ms) {
    if (ms < 0) ms = 0;
    char buf[16];
    if (ms < 10000) {
        int tenths = static_cast<int>(ms / 100); // 0..99
        std::snprintf(buf, sizeof(buf), "%d.%d",
                      tenths / 10, tenths % 10);
        return buf;
    }
    int total_s = static_cast<int>(ms / 1000);
    int m = total_s / 60;
    int s = total_s % 60;
    std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

// ===========================================================================
// HUD components (all in NDC; no width/height dependency)
// ===========================================================================
// Stockfish-centipawn score history plotted in the top-right corner.
// The score-line fill area below is always the human's own colour
// (flip the axis when the human plays black) and an analysis-mode
// dot marks the current replay position.
static void draw_score_graph(const GameState& gs, bool human_plays_white) {
    if (gs.score_history.size() < 2) return;
    glDisable(GL_DEPTH_TEST); glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    float gx0 = 0.55f, gx1 = 0.95f, gy0 = 0.55f, gy1 = 0.95f;
    float gw = gx1-gx0, gh = gy1-gy0;
    float max_s = 5.0f;
    for (float s : gs.score_history) if (std::abs(s) > max_s) max_s = std::abs(s);
    max_s = std::ceil(max_s);

    int n = static_cast<int>(gs.score_history.size());

    // Flip the score axis when the human plays black so the human's
    // colour ends up at the bottom of the graph.
    float sign = human_plays_white ? 1.0f : -1.0f;

    std::vector<float> score_y(n);
    for (int i = 0; i < n; i++)
        score_y[i] = std::max(gy0, std::min(gy1,
            gy0 + gh*0.5f + sign * (gs.score_history[i]/max_s)*gh*0.45f));

    std::vector<float> gv;

    // White fill: score line DOWN to graph bottom (white-advantage area).
    int white_fill_start = 0;
    for (int i = 0; i < n-1; i++) {
        float t0 = float(i)/(n-1), t1 = float(i+1)/(n-1);
        float x0 = gx0+t0*gw, x1 = gx0+t1*gw;
        gv.insert(gv.end(), {x0,score_y[i],0, x1,score_y[i+1],0, x1,gy0,0});
        gv.insert(gv.end(), {x0,score_y[i],0, x1,gy0,0, x0,gy0,0});
    }
    int white_fill_count = static_cast<int>(gv.size()/3) - white_fill_start;

    // Black fill: score line UP to graph top (black-advantage area).
    int black_fill_start = static_cast<int>(gv.size()/3);
    for (int i = 0; i < n-1; i++) {
        float t0 = float(i)/(n-1), t1 = float(i+1)/(n-1);
        float x0 = gx0+t0*gw, x1 = gx0+t1*gw;
        gv.insert(gv.end(), {x0,gy1,0, x1,gy1,0, x1,score_y[i+1],0});
        gv.insert(gv.end(), {x0,gy1,0, x1,score_y[i+1],0, x0,score_y[i],0});
    }
    int black_fill_count = static_cast<int>(gv.size()/3) - black_fill_start;

    int zl_start = static_cast<int>(gv.size()/3);
    float zy = gy0 + gh*0.5f;
    gv.insert(gv.end(), {gx0,zy,0, gx1,zy,0});
    int zc = 2;

    int ls = static_cast<int>(gv.size()/3);
    for (int i = 0; i < n-1; i++) {
        float t0 = float(i)/(n-1), t1 = float(i+1)/(n-1);
        float x0 = gx0+t0*gw, x1 = gx0+t1*gw;
        gv.insert(gv.end(), {x0,score_y[i],0, x1,score_y[i+1],0});
    }
    int lc = (n-1)*2;

    GLuint gvao, gvbo;
    glGenVertexArrays(1, &gvao); glGenBuffers(1, &gvbo);
    glBindVertexArray(gvao);
    glBindBuffer(GL_ARRAY_BUFFER, gvbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(gv.size()*sizeof(float)), gv.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glUseProgram(g_highlight_program);
    Mat4 id = mat4_identity();
    glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"), 1, GL_FALSE, id.m);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);

    float below_r = human_plays_white ? 0.85f : 0.12f;
    float below_g = human_plays_white ? 0.85f : 0.12f;
    float below_b = human_plays_white ? 0.85f : 0.12f;
    float above_r = human_plays_white ? 0.12f : 0.85f;
    float above_g = human_plays_white ? 0.12f : 0.85f;
    float above_b = human_plays_white ? 0.12f : 0.85f;
    glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                below_r, below_g, below_b, 0.8f);
    glDrawArrays(GL_TRIANGLES, white_fill_start, white_fill_count);
    glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                above_r, above_g, above_b, 0.8f);
    glDrawArrays(GL_TRIANGLES, black_fill_start, black_fill_count);

    glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0.5f,0.5f,0.5f,0.4f);
    glLineWidth(1); glDrawArrays(GL_LINES, zl_start, zc);

    // Score line in 50% grey — halfway between the white and black
    // fills so it reads as "neither side".
    glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0.5f,0.5f,0.5f,0.9f);
    glLineWidth(2); glDrawArrays(GL_LINES, ls, lc);

    if (gs.analysis_mode && gs.analysis_index < n) {
        float dt = (n>1) ? float(gs.analysis_index)/(n-1) : 0;
        float dx = gx0+dt*gw;
        float dy = std::max(gy0, std::min(gy1,
            gy0 + gh*0.5f + sign * (gs.score_history[gs.analysis_index]/max_s)*gh*0.45f));
        float dr = 0.012f; int ds = 16;
        int db = static_cast<int>(gv.size()/3);
        float dst = 2.0f*static_cast<float>(M_PI)/ds;
        for (int i = 0; i < ds; i++) {
            float a0 = dst*i, a1 = dst*(i+1);
            gv.insert(gv.end(), {dx,dy,0, dx+std::cos(a0)*dr,dy+std::sin(a0)*dr,0, dx+std::cos(a1)*dr,dy+std::sin(a1)*dr,0});
        }
        glBindBuffer(GL_ARRAY_BUFFER, gvbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(gv.size()*sizeof(float)), gv.data(), GL_STREAM_DRAW);
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 1,1,1,1);
        glDrawArrays(GL_TRIANGLES, db, ds*3);
    }

    glBindVertexArray(0); glDeleteBuffers(1, &gvbo); glDeleteVertexArrays(1, &gvao);

    // Win-percentage labels above each end of the graph.
    float cur = gs.score_history.back();
    float wp_val = 1.0f/(1.0f+std::exp(-cur*0.5f));
    int wpct = static_cast<int>(std::round(wp_val*100)); int bpct = 100-wpct;

    char ps[32];
    std::snprintf(ps, sizeof(ps), "%d%%", wpct); std::string ws = ps;
    std::snprintf(ps, sizeof(ps), "%d%%", bpct); std::string bs = ps;

    std::vector<float> tv;
    float pch_w = 0.022f, pch_h = 0.032f;
    float pty = gy1 + 0.025f;

    add_screen_string(tv, gx0 + 0.01f, pty, pch_w, pch_h, ws);
    int white_verts = static_cast<int>(tv.size() / 5);

    float bw = bs.size() * pch_w * 0.7f;
    add_screen_string(tv, gx1 - bw - 0.01f, pty, pch_w, pch_h, bs);
    int total_verts = static_cast<int>(tv.size() / 5);

    if (total_verts > 0) {
        GLuint tvao, tvbo;
        glGenVertexArrays(1, &tvao); glGenBuffers(1, &tvbo);
        glBindVertexArray(tvao);
        glBindBuffer(GL_ARRAY_BUFFER, tvbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(tv.size() * sizeof(float)),
                     tv.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);

        glUseProgram(g_text_program);
        glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"), 1, GL_FALSE, id.m);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_font_tex);
        glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);

        glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 1, 1, 1, 0.9f);
        glDrawArrays(GL_TRIANGLES, 0, white_verts);
        glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 0.7f, 0.7f, 0.7f, 0.9f);
        glDrawArrays(GL_TRIANGLES, white_verts, total_verts - white_verts);

        glBindVertexArray(0); glDeleteBuffers(1, &tvbo); glDeleteVertexArrays(1, &tvao);
    }
    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}

// Algebraic move list below the score graph. Two columns (white /
// black) per full move number; the currently-selected move during
// analysis mode is tinted yellow.
static void draw_move_list(const GameState& gs) {
    if (gs.snapshots.size() <= 1) return;
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float gx0 = 0.55f, gx1 = 0.95f;
    float graph_bottom = 0.55f;
    float ml_center = (gx0 + gx1) * 0.5f;

    float ch_w = 0.024f, ch_h = 0.036f;
    float line_h = 0.040f;
    float num_w = 0.055f;
    float col_gap = 0.01f;
    float half_row_w = 0.08f;

    int total_moves = static_cast<int>(gs.snapshots.size()) - 1;
    int total_full_moves = (total_moves + 1) / 2;
    int max_lines = 12;
    int first_move = 0;
    if (total_full_moves > max_lines)
        first_move = total_full_moves - max_lines;
    int visible_lines = std::min(total_full_moves - first_move, max_lines);

    float ml_top = graph_bottom - 0.015f;
    float row_w = num_w + half_row_w * 2 + col_gap;
    float ml_x0 = ml_center - row_w * 0.5f;

    // Normal and highlighted entries separated so analysis-mode
    // selection renders in a second draw call with a different colour.
    struct MoveEntry {
        std::vector<float> verts;
        int snapshot_idx;
    };
    std::vector<MoveEntry> normal_entries;
    std::vector<MoveEntry> highlight_entries;

    float y = ml_top;
    for (int move_num = first_move; move_num < first_move + visible_lines; move_num++) {
        int white_snap = move_num * 2 + 1;
        int black_snap = move_num * 2 + 2;

        std::string num_str = std::to_string(move_num + 1) + ".";
        MoveEntry num_entry; num_entry.snapshot_idx = -1;
        add_screen_string(num_entry.verts, ml_x0, y, ch_w, ch_h, num_str);
        normal_entries.push_back(num_entry);

        if (white_snap <= total_moves && white_snap < static_cast<int>(gs.snapshots.size())) {
            std::string alg = uci_to_algebraic(gs.snapshots[white_snap - 1],
                                                gs.snapshots[white_snap].last_move);
            MoveEntry entry; entry.snapshot_idx = white_snap;
            add_screen_string(entry.verts, ml_x0 + num_w, y, ch_w, ch_h, alg);
            if (gs.analysis_mode && gs.analysis_index == white_snap)
                highlight_entries.push_back(entry);
            else
                normal_entries.push_back(entry);
        }

        if (black_snap <= total_moves && black_snap < static_cast<int>(gs.snapshots.size())) {
            std::string alg = uci_to_algebraic(gs.snapshots[black_snap - 1],
                                                gs.snapshots[black_snap].last_move);
            MoveEntry entry; entry.snapshot_idx = black_snap;
            add_screen_string(entry.verts, ml_x0 + num_w + half_row_w + col_gap, y, ch_w, ch_h, alg);
            if (gs.analysis_mode && gs.analysis_index == black_snap)
                highlight_entries.push_back(entry);
            else
                normal_entries.push_back(entry);
        }

        y -= line_h;
    }

    std::vector<float> normal_verts, hl_verts;
    for (auto& e : normal_entries)
        normal_verts.insert(normal_verts.end(), e.verts.begin(), e.verts.end());
    for (auto& e : highlight_entries)
        hl_verts.insert(hl_verts.end(), e.verts.begin(), e.verts.end());

    std::vector<float> all_verts;
    all_verts.insert(all_verts.end(), normal_verts.begin(), normal_verts.end());
    int normal_count = static_cast<int>(normal_verts.size() / 5);
    all_verts.insert(all_verts.end(), hl_verts.begin(), hl_verts.end());
    int hl_count = static_cast<int>(hl_verts.size() / 5);
    int total_count = normal_count + hl_count;

    if (total_count > 0) {
        GLuint mvao, mvbo;
        glGenVertexArrays(1, &mvao); glGenBuffers(1, &mvbo);
        glBindVertexArray(mvao);
        glBindBuffer(GL_ARRAY_BUFFER, mvbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(all_verts.size() * sizeof(float)),
                     all_verts.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);

        glUseProgram(g_text_program);
        Mat4 id = mat4_identity();
        glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"), 1, GL_FALSE, id.m);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_font_tex);
        glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);

        if (normal_count > 0) {
            glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 0.80f, 0.80f, 0.80f, 0.9f);
            glDrawArrays(GL_TRIANGLES, 0, normal_count);
        }
        if (hl_count > 0) {
            glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 1.0f, 0.9f, 0.2f, 1.0f);
            glDrawArrays(GL_TRIANGLES, normal_count, hl_count);
        }

        glBindVertexArray(0);
        glDeleteBuffers(1, &mvbo); glDeleteVertexArrays(1, &mvao);
    }

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}

// Top-centre side-to-move clock panel. Draws an outlined dark panel,
// the side label ("White" / "Black"), and the time remaining in
// M:SS, or S.T for the last ten seconds with a red pulse.
static void draw_clock_widget(const GameState& gs, int64_t clock_ms_remaining,
                              bool clock_side_is_white) {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(g_highlight_program);
    Mat4 id_c = mat4_identity();
    glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"),
                       1, GL_FALSE, id_c.m);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
    glUniform1i(glGetUniformLocation(g_highlight_program, "uUseGradient"), 0);
    glUniform1i(glGetUniformLocation(g_highlight_program, "uUseVertexColor"), 0);

    // Panel NDC rectangle, sized to fit the side label + clock text
    // with a 0.010 NDC pad top/bottom.
    const float cx0 = -0.16f, cx1 = +0.16f;
    const float cy0 =  0.850f, cy1 =  0.985f;

    glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                0.55f, 0.60f, 0.72f, 0.80f);
    {
        std::vector<float> ov;
        push_quad(ov,
                  cx0 - 0.006f, cy0 - 0.008f,
                  cx1 + 0.006f, cy1 + 0.005f);
        GLuint ovao, ovbo;
        glGenVertexArrays(1, &ovao); glGenBuffers(1, &ovbo);
        glBindVertexArray(ovao); glBindBuffer(GL_ARRAY_BUFFER, ovbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(ov.size() * sizeof(float)),
                     ov.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(ov.size() / 3));
        glBindVertexArray(0);
        glDeleteBuffers(1, &ovbo); glDeleteVertexArrays(1, &ovao);
    }

    glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                0.08f, 0.10f, 0.14f, 0.85f);
    {
        std::vector<float> pv;
        push_quad(pv, cx0, cy0, cx1, cy1);
        GLuint pvao, pvbo;
        glGenVertexArrays(1, &pvao); glGenBuffers(1, &pvbo);
        glBindVertexArray(pvao); glBindBuffer(GL_ARRAY_BUFFER, pvbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(pv.size() * sizeof(float)),
                     pv.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(pv.size() / 3));
        glBindVertexArray(0);
        glDeleteBuffers(1, &pvbo); glDeleteVertexArrays(1, &pvao);
    }

    glUseProgram(g_text_program);
    glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"),
                       1, GL_FALSE, id_c.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);

    std::vector<float> cv;
    // add_screen_string treats y as the TOP of the character; offset
    // by the character height so labels sit inside the panel's top
    // edge with a 0.010 NDC pad.
    const float lch = 0.030f;
    const float cch = 0.075f;
    const float label_top_y = cy1 - 0.010f;
    const float clock_top_y = label_top_y - lch - 0.008f;
    {
        const char* label = clock_side_is_white ? "White" : "Black";
        std::string s = label;
        float lcw = 0.022f;
        float lw = s.size() * lcw * 0.7f;
        add_screen_string(cv, -lw * 0.5f, label_top_y, lcw, lch, s);
    }
    int side_end = static_cast<int>(cv.size() / 5);

    std::string clock_text = format_clock_ms(clock_ms_remaining);
    float ccw = 0.055f;
    float cw_total = clock_text.size() * ccw * 0.7f;
    add_screen_string(cv, -cw_total * 0.5f, clock_top_y, ccw, cch, clock_text);
    int clock_end = static_cast<int>(cv.size() / 5);

    if (!cv.empty()) {
        GLuint cvao, cvbo;
        glGenVertexArrays(1, &cvao); glGenBuffers(1, &cvbo);
        glBindVertexArray(cvao); glBindBuffer(GL_ARRAY_BUFFER, cvbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(cv.size() * sizeof(float)),
                     cv.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              5 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                              5 * sizeof(float),
                              (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);

        glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                    0.82f, 0.82f, 0.86f, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, side_end);

        // Warm white normally, pulsing red in the last ten seconds.
        // Pulse phase reuses the monotonic gs.anim_start_time — any
        // time source is fine for sin.
        bool low = clock_ms_remaining < 10000;
        float r = 0.97f, g = 0.97f, b = 0.94f;
        if (low) {
            float t = static_cast<float>(
                (gs.anim_start_time % 1'000'000) / 1.0e6);
            float pulse = 0.5f + 0.5f * std::sin(t * 6.28f * 2.0f);
            r = 1.00f;
            g = 0.20f + pulse * 0.20f;
            b = 0.18f + pulse * 0.15f;
        }
        glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                    r, g, b, 1.0f);
        glDrawArrays(GL_TRIANGLES, side_end, clock_end - side_end);

        glBindVertexArray(0);
        glDeleteBuffers(1, &cvbo); glDeleteVertexArrays(1, &cvao);
    }

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}

// Withdraw flag (bottom-right corner, live game only). Draws a
// brown stick faked as a stack of Lambertian-shaded slices, the
// cloth mesh with per-vertex normal-based lighting from
// flag_build_triangles, and a soft drop shadow.
static void draw_withdraw_flag_widget(const ClothFlag* flag,
                                      int width, int height) {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(g_highlight_program);
    Mat4 id_flag = mat4_identity();
    glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"),
                       1, GL_FALSE, id_flag.m);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
    glUniform1i(glGetUniformLocation(g_highlight_program, "uUseGradient"), 0);

    // 1. Brown stick, rendered as vertical slices with per-slice
    // shading to fake a cylindrical pole — edges dark, centre
    // bright. Cheap and convincing at small sizes.
    const float stick_half_px = 3.0f;
    const float stick_half_x  = stick_half_px * 2.0f / static_cast<float>(width);
    const float stick_top_px  = 6.0f;
    const float stick_top_pad = stick_top_px * 2.0f / static_cast<float>(height);
    const float stick_x       = flag->anchor_x;
    const float stick_y_top   = flag->anchor_y + stick_top_pad;
    // Stick length scales with cloth height: extends one cloth-height
    // below the anchor so the flag sits near the top of the stick
    // with a short bit of pole visible under the free edge.
    const float cloth_h       = flag->rest_v * static_cast<float>(ClothFlag::ROWS - 1);
    const float stick_y_bot   = flag->anchor_y - cloth_h * 2.2f;
    {
        const int SLICES = 9;
        const float base_r = 0.55f, base_g = 0.33f, base_b = 0.13f;
        const float edge_r = 0.22f, edge_g = 0.12f, edge_b = 0.04f;
        for (int i = 0; i < SLICES; ++i) {
            float t0 = static_cast<float>(i)     / SLICES;
            float t1 = static_cast<float>(i + 1) / SLICES;
            float x0 = stick_x + (t0 * 2.0f - 1.0f) * stick_half_x;
            float x1 = stick_x + (t1 * 2.0f - 1.0f) * stick_half_x;
            // shade ~ cosine falloff across the thickness: peaks at
            // 1 in the middle, 0 at both edges.
            float tc = (t0 + t1) * 0.5f;
            float shade = std::sin(tc * static_cast<float>(M_PI));
            float r = edge_r + (base_r - edge_r) * shade;
            float g = edge_g + (base_g - edge_g) * shade;
            float b = edge_b + (base_b - edge_b) * shade;
            glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                        r, g, b, 1.0f);

            std::vector<float> sv;
            push_quad(sv, x0, stick_y_bot, x1, stick_y_top);
            GLuint svao, svbo;
            glGenVertexArrays(1, &svao); glGenBuffers(1, &svbo);
            glBindVertexArray(svao); glBindBuffer(GL_ARRAY_BUFFER, svbo);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(sv.size() * sizeof(float)),
                         sv.data(), GL_STREAM_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                                  3 * sizeof(float), (void*)0);
            glEnableVertexAttribArray(0);
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(sv.size() / 3));
            glBindVertexArray(0);
            glDeleteBuffers(1, &svbo); glDeleteVertexArrays(1, &svao);
        }
    }

    // 2. Cloth mesh from flag_build_triangles: 5 floats per vertex
    // (x, y, r, g, b). Draw a drop shadow first with the same
    // positions offset down+right, then the cloth with per-vertex
    // colour.
    std::vector<float> cloth_verts;
    flag_build_triangles(*flag, cloth_verts);
    const size_t verts_per_stride = 5;
    if (!cloth_verts.empty()) {
        const int n_verts = static_cast<int>(
            cloth_verts.size() / verts_per_stride);

        const float shadow_dx = 4.0f * 2.0f / static_cast<float>(width);
        const float shadow_dy = -4.0f * 2.0f / static_cast<float>(height);
        std::vector<float> shadow_v3;
        shadow_v3.reserve(static_cast<size_t>(n_verts) * 3);
        for (int i = 0; i < n_verts; ++i) {
            shadow_v3.push_back(cloth_verts[i * verts_per_stride + 0] + shadow_dx);
            shadow_v3.push_back(cloth_verts[i * verts_per_stride + 1] + shadow_dy);
            shadow_v3.push_back(0.0f);
        }
        glUniform1i(glGetUniformLocation(g_highlight_program, "uUseVertexColor"), 0);
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    0.0f, 0.0f, 0.0f, 0.35f);
        {
            GLuint fvao, fvbo;
            glGenVertexArrays(1, &fvao); glGenBuffers(1, &fvbo);
            glBindVertexArray(fvao); glBindBuffer(GL_ARRAY_BUFFER, fvbo);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(shadow_v3.size() * sizeof(float)),
                         shadow_v3.data(), GL_STREAM_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                                  3 * sizeof(float), (void*)0);
            glEnableVertexAttribArray(0);
            glDisableVertexAttribArray(1);
            glDrawArrays(GL_TRIANGLES, 0,
                         static_cast<GLsizei>(shadow_v3.size() / 3));
            glBindVertexArray(0);
            glDeleteBuffers(1, &fvbo); glDeleteVertexArrays(1, &fvao);
        }

        std::vector<float> cloth_packed;
        cloth_packed.reserve(static_cast<size_t>(n_verts) * 6);
        for (int i = 0; i < n_verts; ++i) {
            cloth_packed.push_back(cloth_verts[i * verts_per_stride + 0]);
            cloth_packed.push_back(cloth_verts[i * verts_per_stride + 1]);
            cloth_packed.push_back(0.0f);
            cloth_packed.push_back(cloth_verts[i * verts_per_stride + 2]);
            cloth_packed.push_back(cloth_verts[i * verts_per_stride + 3]);
            cloth_packed.push_back(cloth_verts[i * verts_per_stride + 4]);
        }
        glUniform1i(glGetUniformLocation(g_highlight_program, "uUseVertexColor"), 1);
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    1.0f, 1.0f, 1.0f, 1.0f);
        {
            GLuint cvao, cvbo;
            glGenVertexArrays(1, &cvao); glGenBuffers(1, &cvbo);
            glBindVertexArray(cvao); glBindBuffer(GL_ARRAY_BUFFER, cvbo);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(cloth_packed.size() * sizeof(float)),
                         cloth_packed.data(), GL_STREAM_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                                  6 * sizeof(float), (void*)0);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
                                  6 * sizeof(float),
                                  (void*)(3 * sizeof(float)));
            glEnableVertexAttribArray(1);
            glDrawArrays(GL_TRIANGLES, 0, n_verts);
            glBindVertexArray(0);
            glDisableVertexAttribArray(1);
            glDeleteBuffers(1, &cvbo); glDeleteVertexArrays(1, &cvao);
        }
        // Reset so subsequent highlight_program draws don't inherit
        // the vertex-colour path.
        glUniform1i(glGetUniformLocation(g_highlight_program, "uUseVertexColor"), 0);
    }

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}

// Full-screen modal confirming surrender. Backdrop dim, outlined
// panel, Yes (green) + No (red) buttons with hover tints, and the
// "Withdraw from game?" title. withdraw_hover: 0 none, 1 Yes, 2 No.
static void draw_withdraw_confirm_modal(int withdraw_hover) {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(g_highlight_program);
    Mat4 id_wc = mat4_identity();
    glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"),
                       1, GL_FALSE, id_wc.m);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
    glUniform1i(glGetUniformLocation(g_highlight_program, "uUseGradient"), 0);

    // Full-screen backdrop dim.
    glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                0.0f, 0.0f, 0.0f, 0.55f);
    {
        std::vector<float> bv;
        push_quad(bv, -1.0f, -1.0f, 1.0f, 1.0f);
        GLuint bvao, bvbo;
        glGenVertexArrays(1, &bvao); glGenBuffers(1, &bvbo);
        glBindVertexArray(bvao); glBindBuffer(GL_ARRAY_BUFFER, bvbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(bv.size() * sizeof(float)),
                     bv.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(bv.size() / 3));
        glBindVertexArray(0);
        glDeleteBuffers(1, &bvbo); glDeleteVertexArrays(1, &bvao);
    }

    // Outlined panel, slightly larger behind the bg for a 1-cell border.
    glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                0.60f, 0.65f, 0.75f, 0.95f);
    {
        std::vector<float> ov;
        push_quad(ov,
                  WC_PANEL_X0 - 0.006f, WC_PANEL_Y0 - 0.010f,
                  WC_PANEL_X1 + 0.006f, WC_PANEL_Y1 + 0.010f);
        GLuint ovao, ovbo;
        glGenVertexArrays(1, &ovao); glGenBuffers(1, &ovbo);
        glBindVertexArray(ovao); glBindBuffer(GL_ARRAY_BUFFER, ovbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(ov.size() * sizeof(float)),
                     ov.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(ov.size() / 3));
        glBindVertexArray(0);
        glDeleteBuffers(1, &ovbo); glDeleteVertexArrays(1, &ovao);
    }

    glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                0.10f, 0.12f, 0.16f, 0.97f);
    {
        std::vector<float> pv;
        push_quad(pv, WC_PANEL_X0, WC_PANEL_Y0, WC_PANEL_X1, WC_PANEL_Y1);
        GLuint pvao, pvbo;
        glGenVertexArrays(1, &pvao); glGenBuffers(1, &pvbo);
        glBindVertexArray(pvao); glBindBuffer(GL_ARRAY_BUFFER, pvbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(pv.size() * sizeof(float)),
                     pv.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(pv.size() / 3));
        glBindVertexArray(0);
        glDeleteBuffers(1, &pvbo); glDeleteVertexArrays(1, &pvao);
    }

    // Yes button (green, brighter on hover).
    {
        float r = (withdraw_hover == 1) ? 0.30f : 0.20f;
        float g = (withdraw_hover == 1) ? 0.70f : 0.55f;
        float b = (withdraw_hover == 1) ? 0.35f : 0.25f;
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    r, g, b, 0.95f);
        std::vector<float> yv;
        push_quad(yv, WC_YES_X0, WC_YES_Y0, WC_YES_X1, WC_YES_Y1);
        GLuint yvao, yvbo;
        glGenVertexArrays(1, &yvao); glGenBuffers(1, &yvbo);
        glBindVertexArray(yvao); glBindBuffer(GL_ARRAY_BUFFER, yvbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(yv.size() * sizeof(float)),
                     yv.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(yv.size() / 3));
        glBindVertexArray(0);
        glDeleteBuffers(1, &yvbo); glDeleteVertexArrays(1, &yvao);
    }

    // No button (red, brighter on hover).
    {
        float r = (withdraw_hover == 2) ? 0.80f : 0.65f;
        float g = (withdraw_hover == 2) ? 0.28f : 0.22f;
        float b = (withdraw_hover == 2) ? 0.28f : 0.22f;
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    r, g, b, 0.95f);
        std::vector<float> nv;
        push_quad(nv, WC_NO_X0, WC_NO_Y0, WC_NO_X1, WC_NO_Y1);
        GLuint nvao, nvbo;
        glGenVertexArrays(1, &nvao); glGenBuffers(1, &nvbo);
        glBindVertexArray(nvao); glBindBuffer(GL_ARRAY_BUFFER, nvbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(nv.size() * sizeof(float)),
                     nv.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(nv.size() / 3));
        glBindVertexArray(0);
        glDeleteBuffers(1, &nvbo); glDeleteVertexArrays(1, &nvao);
    }

    glUseProgram(g_text_program);
    glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"),
                       1, GL_FALSE, id_wc.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);

    std::vector<float> tv;
    {
        std::string title = "Withdraw from game?";
        float cw = 0.036f, ch = 0.055f;
        float tw = title.size() * cw * 0.7f;
        add_screen_string(tv, -tw * 0.5f, 0.08f, cw, ch, title);
    }
    int title_count = static_cast<int>(tv.size() / 5);

    {
        std::string s = "Yes";
        float cw = 0.030f, ch = 0.045f;
        float sw = s.size() * cw * 0.7f;
        float cx = (WC_YES_X0 + WC_YES_X1) * 0.5f;
        float cy = (WC_YES_Y0 + WC_YES_Y1) * 0.5f;
        add_screen_string(tv, cx - sw * 0.5f, cy + ch * 0.35f, cw, ch, s);
    }
    int yes_end = static_cast<int>(tv.size() / 5);
    int yes_count = yes_end - title_count;

    {
        std::string s = "No";
        float cw = 0.030f, ch = 0.045f;
        float sw = s.size() * cw * 0.7f;
        float cx = (WC_NO_X0 + WC_NO_X1) * 0.5f;
        float cy = (WC_NO_Y0 + WC_NO_Y1) * 0.5f;
        add_screen_string(tv, cx - sw * 0.5f, cy + ch * 0.35f, cw, ch, s);
    }
    int no_end = static_cast<int>(tv.size() / 5);
    int no_count = no_end - yes_end;

    if (!tv.empty()) {
        GLuint tvao, tvbo;
        glGenVertexArrays(1, &tvao); glGenBuffers(1, &tvbo);
        glBindVertexArray(tvao); glBindBuffer(GL_ARRAY_BUFFER, tvbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(tv.size() * sizeof(float)),
                     tv.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              5 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                              5 * sizeof(float),
                              (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);

        glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                    0.95f, 0.95f, 0.95f, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, title_count);
        glDrawArrays(GL_TRIANGLES, title_count, yes_count);
        glDrawArrays(GL_TRIANGLES, title_count + yes_count, no_count);

        glBindVertexArray(0);
        glDeleteBuffers(1, &tvbo); glDeleteVertexArrays(1, &tvao);
    }

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}

// "Pieces missing" modal — same visual style as draw_withdraw_confirm_modal
// (full-screen dim + outlined dark panel) but with one exit button
// instead of Yes/No, and an extra body line listing the squares
// the firmware reports as empty.
void renderer_draw_chessnut_missing_modal(const std::string& squares_msg,
                                          bool exit_hover) {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(g_highlight_program);
    Mat4 id_cm = mat4_identity();
    glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"),
                       1, GL_FALSE, id_cm.m);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
    glUniform1i(glGetUniformLocation(g_highlight_program, "uUseGradient"), 0);

    auto draw_quad = [&](float x0, float y0, float x1, float y1) {
        std::vector<float> v;
        push_quad(v, x0, y0, x1, y1);
        GLuint vao, vbo;
        glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
        glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(v.size() * sizeof(float)),
                     v.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(v.size() / 3));
        glBindVertexArray(0);
        glDeleteBuffers(1, &vbo); glDeleteVertexArrays(1, &vao);
    };

    // Full-screen backdrop dim.
    glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                0.0f, 0.0f, 0.0f, 0.55f);
    draw_quad(-1.0f, -1.0f, 1.0f, 1.0f);

    // Outlined panel (border + inner fill).
    glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                0.60f, 0.65f, 0.75f, 0.95f);
    draw_quad(CMM_PANEL_X0 - 0.006f, CMM_PANEL_Y0 - 0.010f,
              CMM_PANEL_X1 + 0.006f, CMM_PANEL_Y1 + 0.010f);
    glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                0.10f, 0.12f, 0.16f, 0.97f);
    draw_quad(CMM_PANEL_X0, CMM_PANEL_Y0, CMM_PANEL_X1, CMM_PANEL_Y1);

    // Exit-to-Menu button (red, brighter on hover — same palette as
    // the withdraw modal's "No" button so the destructive-action
    // colour is consistent across the app).
    {
        float r = exit_hover ? 0.80f : 0.65f;
        float g = exit_hover ? 0.28f : 0.22f;
        float b = exit_hover ? 0.28f : 0.22f;
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    r, g, b, 0.95f);
        draw_quad(CMM_EXIT_X0, CMM_EXIT_Y0, CMM_EXIT_X1, CMM_EXIT_Y1);
    }

    // Text layer.
    glUseProgram(g_text_program);
    glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"),
                       1, GL_FALSE, id_cm.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);

    std::vector<float> tv;

    // Title.
    {
        std::string title = "Place all pieces on the board";
        float cw = 0.030f, ch = 0.046f;
        float tw = title.size() * cw * 0.7f;
        add_screen_string(tv, -tw * 0.5f, 0.16f, cw, ch, title);
    }
    int title_count = static_cast<int>(tv.size() / 5);

    // Body line 1: list of missing squares (or a fallback).
    {
        std::string body = squares_msg.empty()
            ? std::string("No pieces detected on the board")
            : (std::string("Missing: ") + squares_msg);
        float cw = 0.022f, ch = 0.034f;
        float bw = body.size() * cw * 0.7f;
        add_screen_string(tv, -bw * 0.5f, 0.06f, cw, ch, body);
    }
    int body1_end = static_cast<int>(tv.size() / 5);
    int body1_count = body1_end - title_count;

    // Body line 2: hint about piece batteries.
    {
        std::string hint = "(or check the piece battery)";
        float cw = 0.020f, ch = 0.030f;
        float hw = hint.size() * cw * 0.7f;
        add_screen_string(tv, -hw * 0.5f, 0.005f, cw, ch, hint);
    }
    int body2_end = static_cast<int>(tv.size() / 5);
    int body2_count = body2_end - body1_end;

    // Exit button label.
    {
        std::string s = "Exit to Menu";
        float cw = 0.026f, ch = 0.038f;
        float sw = s.size() * cw * 0.7f;
        float cx = (CMM_EXIT_X0 + CMM_EXIT_X1) * 0.5f;
        float cy = (CMM_EXIT_Y0 + CMM_EXIT_Y1) * 0.5f;
        add_screen_string(tv, cx - sw * 0.5f, cy + ch * 0.35f, cw, ch, s);
    }
    int btn_end = static_cast<int>(tv.size() / 5);
    int btn_count = btn_end - body2_end;

    if (!tv.empty()) {
        GLuint tvao, tvbo;
        glGenVertexArrays(1, &tvao); glGenBuffers(1, &tvbo);
        glBindVertexArray(tvao); glBindBuffer(GL_ARRAY_BUFFER, tvbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(tv.size() * sizeof(float)),
                     tv.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              5 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                              5 * sizeof(float),
                              (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);

        glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                    0.95f, 0.95f, 0.95f, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, title_count);
        glDrawArrays(GL_TRIANGLES, title_count, body1_count);
        glDrawArrays(GL_TRIANGLES, body1_end, body2_count);
        glDrawArrays(GL_TRIANGLES, body2_end, btn_count);

        glBindVertexArray(0);
        glDeleteBuffers(1, &tvbo); glDeleteVertexArrays(1, &tvao);
    }

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}

// Game-over / analysis mode overlay: semi-transparent dark backdrop
// (game-over only — analysis mode keeps the board visible), the
// game result string in gold, a "Back to Menu" button, and in
// analysis mode an additional "Continue Playing" button.
static void draw_game_over_overlay(const GameState& gs,
                                   bool endgame_menu_hover,
                                   bool continue_playing_hover) {
    const bool visible = (gs.game_over && !gs.game_result.empty()) || gs.analysis_mode;
    const bool is_analysis = gs.analysis_mode && !gs.game_over;
    if (!visible) return;

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(g_highlight_program);
    Mat4 id_go = mat4_identity();
    glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"), 1, GL_FALSE, id_go.m);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
    if (!is_analysis) {
        // Result-text backdrop. Skipped in analysis so the user can
        // still see the board while stepping through snapshots.
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    0, 0, 0, 0.5f);
        float bv[] = {-0.6f,-0.12f,0, 0.6f,-0.12f,0, 0.6f,0.12f,0,
                      -0.6f,-0.12f,0, 0.6f,0.12f,0, -0.6f,0.12f,0};
        GLuint gvao, gvbo;
        glGenVertexArrays(1, &gvao); glGenBuffers(1, &gvbo);
        glBindVertexArray(gvao); glBindBuffer(GL_ARRAY_BUFFER, gvbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(bv), bv, GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0); glDeleteBuffers(1, &gvbo); glDeleteVertexArrays(1, &gvao);
    }

    glUseProgram(g_text_program);
    glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"), 1, GL_FALSE, id_go.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);

    std::vector<float> go_verts;
    int go_count = 0;
    if (!is_analysis && !gs.game_result.empty()) {
        float go_cw = 0.045f, go_ch = 0.065f;
        float go_w = gs.game_result.size() * go_cw * 0.7f;
        // Pushed up slightly so the Back to Menu button fits under
        // it inside the backdrop.
        add_screen_string(go_verts, -go_w * 0.5f, 0.085f, go_cw, go_ch, gs.game_result);
        go_count = static_cast<int>(go_verts.size() / 5);
    }

    float btn_cw = 0.028f, btn_ch = 0.042f;
    std::string btn_label = "Back to Menu";
    float btn_lw = btn_label.size() * btn_cw * 0.7f;
    add_screen_string(go_verts, -btn_lw * 0.5f,
                      EG_MENU_BTN_Y - 0.018f, btn_cw, btn_ch, btn_label);
    int btn_label_end = static_cast<int>(go_verts.size() / 5);
    int btn_label_count = btn_label_end - go_count;

    int cont_label_count = 0;
    if (is_analysis) {
        std::string cont_label = "Continue Playing";
        float cont_lw = cont_label.size() * btn_cw * 0.7f;
        add_screen_string(go_verts, -cont_lw * 0.5f,
                          EG_CONT_BTN_Y - 0.018f,
                          btn_cw, btn_ch, cont_label);
        int cont_end = static_cast<int>(go_verts.size() / 5);
        cont_label_count = cont_end - btn_label_end;
    }

    // Button backgrounds (highlight program) before the text layer.
    {
        glUseProgram(g_highlight_program);
        glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"),
                           1, GL_FALSE, id_go.m);
        glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
        glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
        glUniform1i(glGetUniformLocation(g_highlight_program, "uUseGradient"), 0);
        float br = endgame_menu_hover ? 0.35f : 0.22f;
        float bg = endgame_menu_hover ? 0.50f : 0.32f;
        float bb = endgame_menu_hover ? 0.75f : 0.55f;
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    br, bg, bb, 0.55f);
        float bv[] = {
            EG_MENU_BTN_X,                 EG_MENU_BTN_Y - EG_MENU_BTN_H, 0,
            EG_MENU_BTN_X + EG_MENU_BTN_W, EG_MENU_BTN_Y - EG_MENU_BTN_H, 0,
            EG_MENU_BTN_X + EG_MENU_BTN_W, EG_MENU_BTN_Y,                 0,
            EG_MENU_BTN_X,                 EG_MENU_BTN_Y - EG_MENU_BTN_H, 0,
            EG_MENU_BTN_X + EG_MENU_BTN_W, EG_MENU_BTN_Y,                 0,
            EG_MENU_BTN_X,                 EG_MENU_BTN_Y,                 0,
        };
        GLuint bvao, bvbo;
        glGenVertexArrays(1, &bvao); glGenBuffers(1, &bvbo);
        glBindVertexArray(bvao); glBindBuffer(GL_ARRAY_BUFFER, bvbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(bv), bv, GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              3*sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
        glDeleteBuffers(1, &bvbo); glDeleteVertexArrays(1, &bvao);

        // "Continue Playing" — analysis mode only, warmer green
        // tint to distinguish it from the bluish "Back to Menu".
        if (is_analysis) {
            float cr = continue_playing_hover ? 0.30f : 0.20f;
            float cg = continue_playing_hover ? 0.65f : 0.48f;
            float cb = continue_playing_hover ? 0.38f : 0.28f;
            glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                        cr, cg, cb, 0.55f);
            float cv[] = {
                EG_CONT_BTN_X,                 EG_CONT_BTN_Y - EG_CONT_BTN_H, 0,
                EG_CONT_BTN_X + EG_CONT_BTN_W, EG_CONT_BTN_Y - EG_CONT_BTN_H, 0,
                EG_CONT_BTN_X + EG_CONT_BTN_W, EG_CONT_BTN_Y,                 0,
                EG_CONT_BTN_X,                 EG_CONT_BTN_Y - EG_CONT_BTN_H, 0,
                EG_CONT_BTN_X + EG_CONT_BTN_W, EG_CONT_BTN_Y,                 0,
                EG_CONT_BTN_X,                 EG_CONT_BTN_Y,                 0,
            };
            GLuint cvao, cvbo;
            glGenVertexArrays(1, &cvao); glGenBuffers(1, &cvbo);
            glBindVertexArray(cvao); glBindBuffer(GL_ARRAY_BUFFER, cvbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(cv), cv, GL_STREAM_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                                  3*sizeof(float), (void*)0);
            glEnableVertexAttribArray(0);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glBindVertexArray(0);
            glDeleteBuffers(1, &cvbo); glDeleteVertexArrays(1, &cvao);
        }

        glUseProgram(g_text_program);
        glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"),
                           1, GL_FALSE, id_go.m);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_font_tex);
        glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);
    }

    if (go_count > 0 || btn_label_count > 0 || cont_label_count > 0) {
        GLuint gvao, gvbo;
        glGenVertexArrays(1, &gvao); glGenBuffers(1, &gvbo);
        glBindVertexArray(gvao); glBindBuffer(GL_ARRAY_BUFFER, gvbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(go_verts.size()*sizeof(float)),
                     go_verts.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)(3*sizeof(float)));
        glEnableVertexAttribArray(1);

        glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 1.0f, 0.9f, 0.5f, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, go_count);

        float lb = endgame_menu_hover ? 1.0f : 0.92f;
        glUniform4f(glGetUniformLocation(g_text_program, "uColor"), lb, lb, lb, 1.0f);
        glDrawArrays(GL_TRIANGLES, go_count, btn_label_count);

        if (cont_label_count > 0) {
            float lc = continue_playing_hover ? 1.0f : 0.92f;
            glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                        lc, lc, lc, 1.0f);
            glDrawArrays(GL_TRIANGLES,
                         go_count + btn_label_count, cont_label_count);
        }

        glBindVertexArray(0); glDeleteBuffers(1, &gvbo); glDeleteVertexArrays(1, &gvao);
    }

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}

void renderer_draw(GameState& gs, int width, int height,
                   float rot_x, float rot_y, float zoom,
                   bool human_plays_white,
                   bool endgame_menu_hover,
                   bool continue_playing_hover,
                   const ClothFlag* flag, bool draw_flag,
                   bool withdraw_confirm_open, int withdraw_hover,
                   bool draw_clock,
                   int64_t clock_ms_remaining,
                   bool clock_side_is_white,
                   bool cartoon_outline,
                   float shake_x) {
    GLint default_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &default_fbo);

    // When the cartoon outline is on, the 3D-MVP pass renders into
    // an offscreen FBO instead of the default framebuffer; after
    // the 3D draws we run a fullscreen-quad post-process that reads
    // the FBO's color + depth textures and darkens pixels along
    // depth discontinuities. NDC overlays draw AFTER the post-
    // process directly to the default FB so they aren't affected.
    if (cartoon_outline) {
        ensure_scene_fbo(width, height);
    }
    const GLuint main_pass_fbo = cartoon_outline
        ? g_scene_fbo
        : static_cast<GLuint>(default_fbo);

    float aspect = static_cast<float>(width) / static_cast<float>(height);
    float deg2rad = static_cast<float>(M_PI) / 180.0f;
    float rot_z_to_y = -90.0f * deg2rad;

    // shake_x is applied as a view-space x-translation outside the
    // zoom/rotation chain so the entire rendered scene (board + pieces)
    // slides left/right together. Shadows use a separate light-space
    // matrix and intentionally stay put.
    Mat4 view = mat4_multiply(
        mat4_translate(shake_x, 0, -zoom),
        mat4_multiply(mat4_rotate_x(rot_x * deg2rad),
                      mat4_multiply(mat4_rotate_y(rot_y * deg2rad),
                                    mat4_translate(0, -BOARD_Y, 0))));
    Mat4 proj = mat4_perspective(45.0f * deg2rad, aspect, 0.1f, 100.0f);
    Mat4 vp = mat4_multiply(proj, view);

    float cd = zoom;
    float cy = BOARD_Y + cd * std::sin(-rot_x * deg2rad);
    float cxz = cd * std::cos(-rot_x * deg2rad);
    float cx = cxz * std::sin(-rot_y * deg2rad);
    float cz = cxz * std::cos(-rot_y * deg2rad);
    float view_pos[3] = {cx, cy, cz};

    // Light space
    float lx = 0.4f, ly = 1.0f, lz = 0.6f;
    float ll = std::sqrt(lx*lx + ly*ly + lz*lz);
    lx /= ll; ly /= ll; lz /= ll;
    Mat4 lv = mat4_look_at(lx*15, ly*15, lz*15, 0, 0, 0, 0, 0, -1);
    Mat4 lp = mat4_ortho(-10, 10, -10, 10, 1, 40);
    Mat4 light_space = mat4_multiply(lp, lv);

    // --- Shadow pass ---
    glBindFramebuffer(GL_FRAMEBUFFER, g_shadow_fbo);
    glViewport(0, 0, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(2.0f, 4.0f);

    glUseProgram(g_shadow_program);
    GLint slsm = glGetUniformLocation(g_shadow_program, "uLightSpaceMatrix");
    GLint smod = glGetUniformLocation(g_shadow_program, "uModel");
    glUniformMatrix4fv(slsm, 1, GL_FALSE, light_space.m);

    Mat4 board_model = mat4_identity();
    glUniformMatrix4fv(smod, 1, GL_FALSE, board_model.m);
    glBindVertexArray(g_board_vao);
    glDrawArrays(GL_TRIANGLES, 0, g_board_light_count + g_board_dark_count);
    glBindVertexArray(0);

    for (const auto& bp : gs.pieces) {
        if (!bp.alive) continue;
        float wx, wz; square_center(bp.col, bp.row, wx, wz);
        float s = BASE_PIECE_SCALE * piece_scale[bp.type];
        Mat4 pm = piece_model_matrix(wx, wz, s, bp.is_white, rot_z_to_y);
        glUniformMatrix4fv(smod, 1, GL_FALSE, pm.m);
        glBindVertexArray(g_pieces[bp.type].vao);
        glDrawArrays(GL_TRIANGLES, 0, g_pieces[bp.type].num_vertices);
        glBindVertexArray(0);
    }

    glDisable(GL_POLYGON_OFFSET_FILL);
    glBindFramebuffer(GL_FRAMEBUFFER, main_pass_fbo);

    // --- Main pass ---
    glViewport(0, 0, width, height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(g_program);
    glUniformMatrix4fv(glGetUniformLocation(g_program, "uView"), 1, GL_FALSE, view.m);
    glUniformMatrix4fv(glGetUniformLocation(g_program, "uProjection"), 1, GL_FALSE, proj.m);
    glUniformMatrix4fv(glGetUniformLocation(g_program, "uLightSpaceMatrix"), 1, GL_FALSE, light_space.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_shadow_tex);
    glUniform1i(glGetUniformLocation(g_program, "uShadowMap"), 0);
    glUniform3fv(glGetUniformLocation(g_program, "uViewPos"), 1, view_pos);

    float lpos[12] = {0.4f,1,0.6f, -0.5f,0.8f,-0.4f, 0,0.5f,-1, 0,0.5f,1};
    float lcol[12] = {2.5f,2.3f,2, 1,1.1f,1.3f, 0.8f,0.7f,0.6f, 0.8f,0.7f,0.6f};
    glUniform3fv(glGetUniformLocation(g_program, "uLightPositions"), 4, lpos);
    glUniform3fv(glGetUniformLocation(g_program, "uLightColors"), 4, lcol);

    // Board
    float bnm[9]; mat4_normal_matrix(board_model, bnm);
    glUniformMatrix4fv(glGetUniformLocation(g_program, "uModel"), 1, GL_FALSE, board_model.m);
    glUniformMatrix3fv(glGetUniformLocation(g_program, "uNormalMat"), 1, GL_FALSE, bnm);
    glBindVertexArray(g_board_vao);
    set_material(g_program, 0.85f,0.75f,0.55f, 0,0.45f,1, 1);
    glDrawArrays(GL_TRIANGLES, 0, g_board_light_count);
    set_material(g_program, 0.45f,0.25f,0.13f, 0,0.35f,1, 1);
    glDrawArrays(GL_TRIANGLES, g_board_light_count, g_board_dark_count);
    glBindVertexArray(0);

    // Pieces (with AI animation)
    float ai_anim_t = 0.0f;
    if (gs.ai_animating) {
        gint64 now = g_get_monotonic_time();
        float el = static_cast<float>(now - gs.ai_anim_start) / 1000000.0f;
        ai_anim_t = std::min(el / gs.ai_anim_duration, 1.0f);
        ai_anim_t = ai_anim_t * ai_anim_t * (3.0f - 2.0f * ai_anim_t);
    }

    for (const auto& bp : gs.pieces) {
        if (!bp.alive) continue;
        float wx, wz; square_center(bp.col, bp.row, wx, wz);
        float s = BASE_PIECE_SCALE * piece_scale[bp.type];

        bool animating = gs.ai_animating && bp.col == gs.ai_from_col && bp.row == gs.ai_from_row && !bp.is_white;
        if (animating) {
            float fx, fz, tx, tz;
            square_center(gs.ai_from_col, gs.ai_from_row, fx, fz);
            square_center(gs.ai_to_col, gs.ai_to_row, tx, tz);
            wx = fx + (tx - fx) * ai_anim_t;
            wz = fz + (tz - fz) * ai_anim_t;
            float arc = std::sin(ai_anim_t * static_cast<float>(M_PI)) * 0.3f;
            Mat4 orient = mat4_rotate_x(rot_z_to_y);
            orient = mat4_multiply(mat4_rotate_y(static_cast<float>(M_PI)), orient);
            Mat4 pm = mat4_multiply(mat4_translate(wx, BOARD_Y + s + arc, wz),
                                    mat4_multiply(mat4_scale(s,s,s), orient));
            set_material(g_program, 0.02f,0.02f,0.02f, 0,0.35f,1, 0);
            draw_with_model(g_program, pm, g_pieces[bp.type].vao, g_pieces[bp.type].num_vertices);
            continue;
        }

        Mat4 pm = piece_model_matrix(wx, wz, s, bp.is_white, rot_z_to_y);
        if (bp.is_white) set_material(g_program, 0.92f,0.88f,0.78f, 0,0.28f,1, 0);
        else set_material(g_program, 0.02f,0.02f,0.02f, 0,0.35f,1, 0);
        draw_with_model(g_program, pm, g_pieces[bp.type].vao, g_pieces[bp.type].num_vertices);
    }

    // Captured pieces
    {
        float cs = 0.30f;
        int wc = 0, bc = 0;
        for (const auto& bp : gs.pieces) {
            if (bp.alive) continue;
            float s = cs * piece_scale[bp.type];
            int& cnt = bp.is_white ? wc : bc;
            int ri = cnt / 2, ci = cnt % 2;
            float px, pz;
            if (bp.is_white) { px = 5.2f + ci*0.7f; pz = 3.5f - ri*0.7f; }
            else { px = -5.2f - ci*0.7f; pz = -3.5f + ri*0.7f; }
            Mat4 pm = piece_model_matrix(px, pz, s, bp.is_white, rot_z_to_y);
            if (bp.is_white) set_material(g_program, 0.7f,0.65f,0.55f, 0,0.4f,0.7f, 0);
            else set_material(g_program, 0.02f,0.02f,0.02f, 0,0.45f,0.7f, 0);
            draw_with_model(g_program, pm, g_pieces[bp.type].vao, g_pieces[bp.type].num_vertices);
            cnt++;
        }
    }

    // --- AI arrow ---
    if (gs.ai_animating) {
        glUseProgram(g_highlight_program);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        float ay = BOARD_Y + 0.01f;
        float fx, fz, tx, tz;
        square_center(gs.ai_from_col, gs.ai_from_row, fx, fz);
        square_center(gs.ai_to_col, gs.ai_to_row, tx, tz);
        float dx = tx-fx, dz = tz-fz;
        float len = std::sqrt(dx*dx+dz*dz);
        float nx = -dz/len*0.06f, nz = dx/len*0.06f;
        float hl = 0.15f, hw = 0.12f;
        float hx = tx - dx/len*hl, hz = tz - dz/len*hl;
        float hnx = -dz/len*hw, hnz = dx/len*hw;
        std::vector<float> av = {
            fx+nx,ay,fz+nz, fx-nx,ay,fz-nz, hx+nx,ay,hz+nz,
            fx-nx,ay,fz-nz, hx-nx,ay,hz-nz, hx+nx,ay,hz+nz,
            hx+hnx,ay,hz+hnz, hx-hnx,ay,hz-hnz, tx,ay,tz
        };
        GLuint avao, avbo;
        glGenVertexArrays(1, &avao); glGenBuffers(1, &avbo);
        glBindVertexArray(avao);
        glBindBuffer(GL_ARRAY_BUFFER, avbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(av.size()*sizeof(float)), av.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"), 1, GL_FALSE, vp.m);
        glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
        glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0.2f,0.5f,1,0.7f);
        glDrawArrays(GL_TRIANGLES, 0, 9);
        glBindVertexArray(0); glDeleteBuffers(1, &avbo); glDeleteVertexArrays(1, &avao);
        glDepthMask(GL_TRUE); glDisable(GL_BLEND);
    }

    // --- Highlights ---
    glUseProgram(g_highlight_program);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    GLint lmvp = glGetUniformLocation(g_highlight_program, "uMVP");
    GLint lcol_loc = glGetUniformLocation(g_highlight_program, "uColor");
    GLint lin = glGetUniformLocation(g_highlight_program, "uInnerRadius");
    GLint lout = glGetUniformLocation(g_highlight_program, "uOuterRadius");
    float hy = BOARD_Y + 0.005f;

    float anim_t = 0.0f;
    if (gs.selected_col >= 0) {
        gint64 now = g_get_monotonic_time();
        anim_t = static_cast<float>(now - gs.anim_start_time) / 1000000.0f;
    }
    float cycle = 1.2f;
    float phase = std::fmod(anim_t, cycle) / cycle;
    float pulse = 1.0f - phase;
    pulse = pulse * pulse * (3.0f - 2.0f * pulse);

    if (gs.selected_col >= 0) {
        float sx, sz; square_center(gs.selected_col, gs.selected_row, sx, sz);
        Mat4 mvp = mat4_multiply(vp, mat4_translate(sx, hy, sz));
        glUniformMatrix4fv(lmvp, 1, GL_FALSE, mvp.m);
        glUniform4f(lcol_loc, 0.3f,0.6f,1, 0.5f+pulse*0.4f);
        glUniform1f(lin, 0.28f+pulse*0.10f);
        glUniform1f(lout, 0.40f+pulse*0.08f);
        glBindVertexArray(g_ring_vao); glDrawArrays(GL_TRIANGLES, 0, g_ring_vertex_count); glBindVertexArray(0);
    }

    float mi = 0.22f+pulse*0.08f, mo = 0.36f+pulse*0.06f, ma = 0.4f+pulse*0.3f;
    for (const auto& [mc, mr] : gs.valid_moves) {
        float mx, mz; square_center(mc, mr, mx, mz);
        Mat4 mvp = mat4_multiply(vp, mat4_translate(mx, hy, mz));
        glUniformMatrix4fv(lmvp, 1, GL_FALSE, mvp.m);
        int tgt = gs.grid[mr][mc];
        if (tgt >= 0) glUniform4f(lcol_loc, 1,0.3f,0.3f,ma);
        else glUniform4f(lcol_loc, 0.2f,0.5f,1,ma);
        glUniform1f(lin, mi); glUniform1f(lout, mo);
        glBindVertexArray(g_disc_vao); glDrawArrays(GL_TRIANGLES, 0, g_disc_vertex_count); glBindVertexArray(0);
    }
    glDepthMask(GL_TRUE); glDisable(GL_BLEND);

    // --- Board coordinate labels (a-h, 1-8) using font texture ---
    {
        glUseProgram(g_text_program);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_font_tex);
        glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);
        glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"), 1, GL_FALSE, vp.m);
        glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 0.85f, 0.80f, 0.70f, 0.8f);

        glBindVertexArray(g_label_vao);
        glDrawArrays(GL_TRIANGLES, 0, g_label_vertex_count);
        glBindVertexArray(0);

        glDepthMask(GL_TRUE); glDisable(GL_BLEND);
    }

    // --- Cartoon outline post-process ---
    // When enabled, the 3D-MVP draws above rendered into g_scene_fbo
    // instead of the default FB. The shared helper samples those
    // textures and writes the darkened-edge result to the default FB;
    // after this the NDC overlays continue to draw on the default FB
    // as they always have.
    if (cartoon_outline) {
        run_outline_post_process(default_fbo, width, height);
    }

    draw_score_graph(gs, human_plays_white);
    draw_move_list(gs);

    if (draw_clock) {
        draw_clock_widget(gs, clock_ms_remaining, clock_side_is_white);
    }

    if (draw_flag && flag != nullptr && !flag->p.empty()) {
        draw_withdraw_flag_widget(flag, width, height);
    }

    if (withdraw_confirm_open) {
        draw_withdraw_confirm_modal(withdraw_hover);
    }

    draw_game_over_overlay(gs, endgame_menu_hover, continue_playing_hover);
}

// ===========================================================================
// Menu screen
// ===========================================================================
// Menu physics lives in menu_physics.cpp. Menu input (ray-pick,
// throw impulse, button hit-test) lives in menu_input.cpp. Only the
// menu renderer itself stays here — it needs the renderer-owned GL
// globals (g_program, g_pieces, scene FBO, text atlas, …).

void renderer_draw_menu(const std::vector<PhysicsPiece>& pieces,
                        int width, int height, float time,
                        int hover_button,
                        bool cartoon_outline,
                        bool chessnut_connected) {
    // Button layout (BTN_*) defined in menu_input.h so it stays in
    // sync with menu_hit_test's click regions.
    using namespace menu_ui;

    GLint default_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &default_fbo);

    // When the outline is requested, the 3D pass renders into the
    // scene FBO so the post-process has a depth buffer to sample.
    // NDC UI overlays (title, buttons, labels) still go to the
    // default FB after the post-process so they don't get outlined.
    if (cartoon_outline) {
        ensure_scene_fbo(width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, g_scene_fbo);
    }

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glViewport(0, 0, width, height);

    float aspect = static_cast<float>(width) / static_cast<float>(height);
    float deg2rad = static_cast<float>(M_PI) / 180.0f;

    float cam_angle = time * 15.0f, cam_pitch = 25.0f, cam_dist = 12.0f;
    Mat4 view = mat4_multiply(mat4_translate(0, 0, -cam_dist),
        mat4_multiply(mat4_rotate_x(cam_pitch * deg2rad), mat4_rotate_y(cam_angle * deg2rad)));
    Mat4 proj = mat4_perspective(45.0f * deg2rad, aspect, 0.1f, 100.0f);

    float cy = cam_dist * std::sin(-cam_pitch * deg2rad);
    float cxz = cam_dist * std::cos(-cam_pitch * deg2rad);
    float cx = cxz * std::sin(-cam_angle * deg2rad);
    float cz = cxz * std::cos(-cam_angle * deg2rad);
    float vp_arr[3] = {cx, cy, cz};

    glUseProgram(g_program);
    glUniformMatrix4fv(glGetUniformLocation(g_program, "uView"), 1, GL_FALSE, view.m);
    glUniformMatrix4fv(glGetUniformLocation(g_program, "uProjection"), 1, GL_FALSE, proj.m);
    Mat4 dummy_lsm = mat4_identity();
    glUniformMatrix4fv(glGetUniformLocation(g_program, "uLightSpaceMatrix"), 1, GL_FALSE, dummy_lsm.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_shadow_tex);
    glUniform1i(glGetUniformLocation(g_program, "uShadowMap"), 0);
    glUniform3fv(glGetUniformLocation(g_program, "uViewPos"), 1, vp_arr);

    float lpos[12] = {0.4f,1,0.6f, -0.5f,0.8f,-0.4f, 0,0.5f,-1, 0,0.5f,1};
    float lcol[12] = {3,2.8f,2.5f, 1.2f,1.3f,1.5f, 0.8f,0.7f,0.6f, 0.8f,0.7f,0.6f};
    glUniform3fv(glGetUniformLocation(g_program, "uLightPositions"), 4, lpos);
    glUniform3fv(glGetUniformLocation(g_program, "uLightColors"), 4, lcol);

    for (const auto& p : pieces) {
        float s = BASE_PIECE_SCALE * piece_scale[p.type] * p.scale / 0.35f;
        Mat4 rot = mat4_multiply(mat4_rotate_z(p.rot_z * deg2rad),
            mat4_multiply(mat4_rotate_y(p.rot_y * deg2rad), mat4_rotate_x(p.rot_x * deg2rad)));
        Mat4 pm = mat4_multiply(mat4_translate(p.x, p.y, p.z), mat4_multiply(mat4_scale(s,s,s), rot));
        bool is_white = (static_cast<int>(&p - &pieces[0]) % 2 == 0);
        if (is_white) set_material(g_program, 0.92f,0.88f,0.78f, 0,0.28f,1, 0);
        else set_material(g_program, 0.02f,0.02f,0.02f, 0,0.35f,1, 0);
        float nm[9]; mat4_normal_matrix(pm, nm);
        glUniformMatrix4fv(glGetUniformLocation(g_program, "uModel"), 1, GL_FALSE, pm.m);
        glUniformMatrix3fv(glGetUniformLocation(g_program, "uNormalMat"), 1, GL_FALSE, nm);
        glBindVertexArray(g_pieces[p.type].vao);
        glDrawArrays(GL_TRIANGLES, 0, g_pieces[p.type].num_vertices);
        glBindVertexArray(0);
    }

    if (cartoon_outline) {
        run_outline_post_process(default_fbo, width, height);
    }

    // --- UI overlay ---
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(g_text_program);
    Mat4 id = mat4_identity();
    glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"), 1, GL_FALSE, id.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);

    std::vector<float> ui_verts;
    float tcw = 0.07f, tch = 0.10f;
    std::string title = "3D CHESS";
    float tw = title.size() * tcw * 0.7f;
    add_screen_string(ui_verts, -tw*0.5f, 0.35f, tcw, tch, title);
    int title_count = static_cast<int>(ui_verts.size() / 5);

    // Subtitle is replaced by the Multiplayer button when a
    // Chessnut Move board is paired — keeps the layout below
    // unchanged whether or not the new button is present.
    float scw = 0.018f, sch = 0.028f;
    int subtitle_end = static_cast<int>(ui_verts.size() / 5);
    if (!chessnut_connected) {
        std::string subtitle = "Play against stockfish";
        float sw = subtitle.size() * scw * 0.7f;
        add_screen_string(ui_verts, -sw*0.5f, 0.22f, scw, sch, subtitle);
        subtitle_end = static_cast<int>(ui_verts.size() / 5);
    }

    float bcw = 0.028f, bch = 0.042f;
    int multi_end = subtitle_end;
    if (chessnut_connected) {
        std::string mp_text = "Multiplayer";
        float mtw = mp_text.size() * bcw * 0.7f;
        add_screen_string(ui_verts, -mtw*0.5f,
                          BTN_MULTIPLAYER_Y - 0.018f,
                          bcw, bch, mp_text);
        multi_end = static_cast<int>(ui_verts.size() / 5);
    }
    std::string start_text = "Start Game";
    float stw = start_text.size() * bcw * 0.7f;
    add_screen_string(ui_verts, -stw*0.5f, BTN_START_Y - 0.018f, bcw, bch, start_text);
    int start_end = static_cast<int>(ui_verts.size() / 5);

    std::string ch_text = "Challenges";
    float chw = ch_text.size() * bcw * 0.7f;
    add_screen_string(ui_verts, -chw*0.5f, BTN_CHALLENGE_Y - 0.018f, bcw, bch, ch_text);
    int ch_end = static_cast<int>(ui_verts.size() / 5);

    std::string opt_text = "Options";
    float otw = opt_text.size() * bcw * 0.7f;
    add_screen_string(ui_verts, -otw*0.5f, BTN_OPTIONS_Y - 0.018f, bcw, bch, opt_text);
    int opt_end = static_cast<int>(ui_verts.size() / 5);

#ifndef __EMSCRIPTEN__
    std::string quit_text = "Quit";
    float qtw = quit_text.size() * bcw * 0.7f;
    add_screen_string(ui_verts, -qtw*0.5f, BTN_QUIT_Y - 0.018f, bcw, bch, quit_text);
    int quit_end = static_cast<int>(ui_verts.size() / 5);
#endif

    GLuint uvao, uvbo;
    glGenVertexArrays(1, &uvao); glGenBuffers(1, &uvbo);
    glBindVertexArray(uvao); glBindBuffer(GL_ARRAY_BUFFER, uvbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(ui_verts.size()*sizeof(float)), ui_verts.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(1);

    // Button backgrounds
    glUseProgram(g_highlight_program);
    glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"), 1, GL_FALSE, id.m);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
    {
        std::vector<float> bg;
        auto push_quad = [&](float top) {
            float v[18] = {
                BTN_X, top - BTN_H, 0, BTN_X + BTN_W, top - BTN_H, 0,
                BTN_X + BTN_W, top, 0,
                BTN_X, top - BTN_H, 0, BTN_X + BTN_W, top, 0,
                BTN_X, top, 0
            };
            bg.insert(bg.end(), v, v + 18);
        };
        push_quad(BTN_START_Y);
        push_quad(BTN_CHALLENGE_Y);
        push_quad(BTN_OPTIONS_Y);
        push_quad(BTN_QUIT_Y);
        if (chessnut_connected) push_quad(BTN_MULTIPLAYER_Y);
        GLuint bvao, bvbo;
        glGenVertexArrays(1, &bvao); glGenBuffers(1, &bvbo);
        glBindVertexArray(bvao); glBindBuffer(GL_ARRAY_BUFFER, bvbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(bg.size() * sizeof(float)),
                     bg.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0.2f,0.4f,0.8f, hover_button==1?0.5f:0.3f);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0.2f,0.6f,0.3f, hover_button==3?0.5f:0.3f);
        glDrawArrays(GL_TRIANGLES, 6, 6);
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0.5f,0.4f,0.7f, hover_button==4?0.5f:0.3f);
        glDrawArrays(GL_TRIANGLES, 12, 6);
#ifndef __EMSCRIPTEN__
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0.6f,0.15f,0.15f, hover_button==2?0.5f:0.3f);
        glDrawArrays(GL_TRIANGLES, 18, 6);
#endif
        if (chessnut_connected) {
            // Multiplayer button — warm amber so it stands out
            // visually as the "physical board involved" option.
            glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                        0.85f, 0.55f, 0.20f, hover_button == 5 ? 0.55f : 0.35f);
            glDrawArrays(GL_TRIANGLES, 24, 6);
        }
        glBindVertexArray(0); glDeleteBuffers(1, &bvbo); glDeleteVertexArrays(1, &bvao);
    }

    glUseProgram(g_text_program);
    glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"), 1, GL_FALSE, id.m);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);
    glBindVertexArray(uvao);

    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 1,0.9f,0.6f,1);
    glDrawArrays(GL_TRIANGLES, 0, title_count);
    if (subtitle_end > title_count) {
        glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 0.7f,0.7f,0.7f,0.8f);
        glDrawArrays(GL_TRIANGLES, title_count, subtitle_end - title_count);
    }
    if (multi_end > subtitle_end) {
        float mi = hover_button == 5 ? 1.0f : 0.92f;
        glUniform4f(glGetUniformLocation(g_text_program, "uColor"), mi, mi, mi, 1);
        glDrawArrays(GL_TRIANGLES, subtitle_end, multi_end - subtitle_end);
    }
    float si = hover_button==1 ? 1.0f : 0.85f;
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), si,si,si,1);
    glDrawArrays(GL_TRIANGLES, multi_end, start_end - multi_end);
    float ci = hover_button==3 ? 1.0f : 0.85f;
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), ci,ci,ci,1);
    glDrawArrays(GL_TRIANGLES, start_end, ch_end - start_end);
    float oi = hover_button==4 ? 1.0f : 0.85f;
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), oi,oi,oi,1);
    glDrawArrays(GL_TRIANGLES, ch_end, opt_end - ch_end);
#ifndef __EMSCRIPTEN__
    float qi = hover_button==2 ? 1.0f : 0.85f;
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), qi,qi,qi,1);
    glDrawArrays(GL_TRIANGLES, opt_end, quit_end - opt_end);
#endif

    glBindVertexArray(0); glDeleteBuffers(1, &uvbo); glDeleteVertexArrays(1, &uvao);
    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}


