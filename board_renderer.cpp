#include "board_renderer.h"
#include "mat4.h"
#include "shader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <epoxy/gl.h>
#include <glib.h>
#include <cairo.h>
#include <pango/pangocairo.h>

// ---------------------------------------------------------------------------
// GL state
// ---------------------------------------------------------------------------
static PieceGPU g_pieces[PIECE_COUNT];
static GLuint g_board_vao = 0, g_board_vbo = 0;
static int g_board_light_count = 0, g_board_dark_count = 0;
static GLuint g_program = 0;
static GLuint g_highlight_program = 0;
static GLuint g_shadow_program = 0;
static GLuint g_shadow_fbo = 0, g_shadow_tex = 0;
static constexpr int SHADOW_MAP_SIZE = 4096;
static GLuint g_disc_vao = 0, g_disc_vbo = 0;
static int g_disc_vertex_count = 0;
static GLuint g_ring_vao = 0, g_ring_vbo = 0;

// Font rendering
static GLuint g_text_program = 0;
static GLuint g_font_tex = 0;
static GLuint g_label_vao = 0, g_label_vbo = 0;
static int g_label_vertex_count = 0;
static int g_ring_vertex_count = 0;

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
            auto& buf = ((row + col) % 2 != 0) ? light : dark;
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
    std::vector<float> buf = model.build_vertex_buffer();
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
// ---------------------------------------------------------------------------
// Font atlas: renders "a"-"h" and "1"-"8" into a single-channel texture
// Layout: 16 cells in a row, each CELL_SIZE x CELL_SIZE
// Index 0-7 = 'a'-'h', 8-15 = '1'-'8'
// ---------------------------------------------------------------------------
static constexpr int CELL_SIZE = 64;
static constexpr int ATLAS_W = 16 * CELL_SIZE; // 1024
static constexpr int ATLAS_H = CELL_SIZE;       // 64

static void build_font_atlas() {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_A8, ATLAS_W, ATLAS_H);
    cairo_t* cr = cairo_create(surface);

    // Clear to transparent
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* font = pango_font_description_from_string("Sans Bold 38");
    pango_layout_set_font_description(layout, font);
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);

    cairo_set_source_rgba(cr, 1, 1, 1, 1);

    const char* labels[16] = {"a","b","c","d","e","f","g","h","1","2","3","4","5","6","7","8"};

    for (int i = 0; i < 16; i++) {
        pango_layout_set_text(layout, labels[i], -1);
        int pw, ph;
        pango_layout_get_pixel_size(layout, &pw, &ph);
        float ox = i * CELL_SIZE + (CELL_SIZE - pw) * 0.5f;
        float oy = (CELL_SIZE - ph) * 0.5f;
        cairo_move_to(cr, ox, oy);
        pango_cairo_show_layout(cr, layout);
    }

    cairo_surface_flush(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);

    glGenTextures(1, &g_font_tex);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ATLAS_W, ATLAS_H, 0, GL_RED, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    pango_font_description_free(font);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

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

    auto add_quad = [&](float cx, float cz, int atlas_idx) {
        float u0 = static_cast<float>(atlas_idx) / 16.0f;
        float u1 = static_cast<float>(atlas_idx + 1) / 16.0f;
        float hw = char_w * 0.5f, hh = char_h * 0.5f;

        // 4 corners in local (right, up) coords: BL, BR, TR, TL
        float lx[4] = {-hw,  hw,  hw, -hw};
        float ly[4] = {-hh, -hh,  hh,  hh};
        float wu[4] = {u0, u1, u1, u0};
        float wv[4] = {1,  1,  0,  0}; // v=1 at bottom, v=0 at top

        // World positions
        float wx[4], wz[4];
        for (int i = 0; i < 4; i++) {
            wx[i] = cx + lx[i] * rx + ly[i] * ux;
            wz[i] = cz + lx[i] * rz + ly[i] * uz;
        }

        // Two triangles: BL-BR-TR, BL-TR-TL
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
        int letter_idx = 7 - col; // 'a' at col 7 (+X = screen left)
        add_quad(cx, -4.0f * SQ - margin, letter_idx);
        add_quad(cx,  4.0f * SQ + margin, letter_idx);
    }

    // Rank numbers (1-8) along both edges
    for (int row = 0; row < 8; row++) {
        float cx_unused, cz;
        square_center(0, row, cx_unused, cz);
        int num_idx = 8 + row; // '1' at index 8, '8' at index 15
        add_quad( 4.0f * SQ + margin, cz, num_idx);
        add_quad(-4.0f * SQ - margin, cz, num_idx);
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

    // Shadow map
    glGenFramebuffers(1, &g_shadow_fbo); glGenTextures(1, &g_shadow_tex);
    glBindTexture(GL_TEXTURE_2D, g_shadow_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float bc[] = {1,1,1,1}; glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, bc);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, g_shadow_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_shadow_tex, 0);
    glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE);
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
void renderer_draw(GameState& gs, int width, int height,
                   float rot_x, float rot_y, float zoom) {
    GLint default_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &default_fbo);

    float aspect = static_cast<float>(width) / static_cast<float>(height);
    float deg2rad = static_cast<float>(M_PI) / 180.0f;
    float rot_z_to_y = -90.0f * deg2rad;

    Mat4 view = mat4_multiply(
        mat4_translate(0, 0, -zoom),
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
    glBindFramebuffer(GL_FRAMEBUFFER, default_fbo);

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

    // --- Score graph ---
    if (gs.score_history.size() >= 2) {
        glDisable(GL_DEPTH_TEST); glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        float gx0 = 0.55f, gx1 = 0.95f, gy0 = 0.55f, gy1 = 0.95f;
        float gw = gx1-gx0, gh = gy1-gy0;
        float max_s = 5.0f;
        for (float s : gs.score_history) if (std::abs(s) > max_s) max_s = std::abs(s);
        max_s = std::ceil(max_s);

        std::vector<float> gv;
        gv.insert(gv.end(), {gx0,gy0,0, gx1,gy0,0, gx1,gy1,0, gx0,gy0,0, gx1,gy1,0, gx0,gy1,0});
        int bgc = 6;
        float zy = gy0 + gh*0.5f;
        gv.insert(gv.end(), {gx0,zy,0, gx1,zy,0});
        int zc = 2;
        int ls = bgc + zc;
        int n = static_cast<int>(gs.score_history.size());
        for (int i = 0; i < n-1; i++) {
            float t0 = float(i)/(n-1), t1 = float(i+1)/(n-1);
            float x0 = gx0+t0*gw, x1 = gx0+t1*gw;
            float y0 = std::max(gy0, std::min(gy1, gy0+gh*0.5f+(gs.score_history[i]/max_s)*gh*0.45f));
            float y1 = std::max(gy0, std::min(gy1, gy0+gh*0.5f+(gs.score_history[i+1]/max_s)*gh*0.45f));
            gv.insert(gv.end(), {x0,y0,0, x1,y1,0});
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
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0,0,0,0.6f);
        glDrawArrays(GL_TRIANGLES, 0, bgc);
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0.5f,0.5f,0.5f,0.5f);
        glLineWidth(1); glDrawArrays(GL_LINES, bgc, zc);
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0.3f,0.8f,0.3f,0.9f);
        glLineWidth(2); glDrawArrays(GL_LINES, ls, lc);

        // Analysis dot
        if (gs.analysis_mode && gs.analysis_index < n) {
            float dt = (n>1) ? float(gs.analysis_index)/(n-1) : 0;
            float dx = gx0+dt*gw;
            float dy = std::max(gy0, std::min(gy1, gy0+gh*0.5f+(gs.score_history[gs.analysis_index]/max_s)*gh*0.45f));
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

        // Win percentage text
        float cur = gs.score_history.back();
        float wp = 1.0f/(1.0f+std::exp(-cur*0.5f));
        int wpct = static_cast<int>(std::round(wp*100)); int bpct = 100-wpct;

        auto add_digit = [](std::vector<float>& v, float cx, float cy, float w, float h, char ch) {
            float hw=w*0.5f, hh=h*0.5f;
            float segs[7][4] = {{-hw,hh,hw,hh},{hw,hh,hw,0},{hw,0,hw,-hh},{-hw,-hh,hw,-hh},{-hw,-hh,-hw,0},{-hw,0,-hw,hh},{-hw,0,hw,0}};
            int pat[11][7] = {{1,1,1,1,1,1,0},{0,1,1,0,0,0,0},{1,1,0,1,1,0,1},{1,1,1,1,0,0,1},{0,1,1,0,0,1,1},{1,0,1,1,0,1,1},{1,0,1,1,1,1,1},{1,1,1,0,0,0,0},{1,1,1,1,1,1,1},{1,1,1,1,0,1,1},{0,0,0,0,0,0,1}};
            int idx = (ch>='0'&&ch<='9') ? ch-'0' : (ch=='-'?10:-1);
            if (idx<0) return;
            for (int s=0;s<7;s++) if (pat[idx][s]) { v.push_back(cx+segs[s][0]);v.push_back(cy+segs[s][1]);v.push_back(0); v.push_back(cx+segs[s][2]);v.push_back(cy+segs[s][3]);v.push_back(0); }
        };
        auto add_pct = [](std::vector<float>& v, float cx, float cy, float w, float h) {
            float hw=w*0.4f,hh=h*0.4f,r=w*0.15f;
            v.insert(v.end(),{cx-hw,cy-hh,0, cx+hw,cy+hh,0});
            for (float ox : {cx-hw*0.6f, cx+hw*0.6f}) { float oy = (ox<cx)?cy+hh*0.6f:cy-hh*0.6f;
                v.insert(v.end(),{ox-r,oy-r,0,ox+r,oy-r,0, ox+r,oy-r,0,ox+r,oy+r,0, ox+r,oy+r,0,ox-r,oy+r,0, ox-r,oy+r,0,ox-r,oy-r,0}); }
        };

        char ps[32]; std::snprintf(ps,sizeof(ps),"%d",wpct); std::string ws=ps;
        std::snprintf(ps,sizeof(ps),"%d",bpct); std::string bs=ps;
        std::vector<float> tv; float dw=0.018f,dh=0.028f,sp=0.024f,ty=gy1+0.025f;
        float txp = gx0+0.01f;
        for (char c : ws) { add_digit(tv,txp,ty,dw,dh,c); txp+=sp; }
        add_pct(tv,txp,ty,dw,dh);
        int wtl = static_cast<int>(tv.size()/3)/2;
        txp = gx1-0.01f-(float(bs.size())+1)*sp;
        int bts = static_cast<int>(tv.size()/3);
        for (char c : bs) { add_digit(tv,txp,ty,dw,dh,c); txp+=sp; }
        add_pct(tv,txp,ty,dw,dh);
        int btl = (static_cast<int>(tv.size()/3)-bts)/2;

        if (!tv.empty()) {
            GLuint tvao, tvbo;
            glGenVertexArrays(1,&tvao); glGenBuffers(1,&tvbo);
            glBindVertexArray(tvao); glBindBuffer(GL_ARRAY_BUFFER,tvbo);
            glBufferData(GL_ARRAY_BUFFER,static_cast<GLsizeiptr>(tv.size()*sizeof(float)),tv.data(),GL_STREAM_DRAW);
            glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,3*sizeof(float),(void*)0);
            glEnableVertexAttribArray(0);
            glLineWidth(1.5f);
            glUniform4f(glGetUniformLocation(g_highlight_program,"uColor"),1,1,1,0.9f);
            glDrawArrays(GL_LINES,0,wtl*2);
            glUniform4f(glGetUniformLocation(g_highlight_program,"uColor"),0.7f,0.7f,0.7f,0.9f);
            glDrawArrays(GL_LINES,bts,btl*2);
            glBindVertexArray(0); glDeleteBuffers(1,&tvbo); glDeleteVertexArrays(1,&tvao);
        }
        glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
    }
}
