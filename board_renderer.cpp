#include "board_renderer.h"
#include "chess_rules.h"
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
// Font atlas: full ASCII printable range (32-126) = 95 characters
// ---------------------------------------------------------------------------
static constexpr int CELL_SIZE = 48;
static constexpr int ATLAS_COLS = 16;
static constexpr int ATLAS_ROWS = 6; // ceil(95/16)
static constexpr int ATLAS_W = ATLAS_COLS * CELL_SIZE;
static constexpr int ATLAS_H = ATLAS_ROWS * CELL_SIZE;
static constexpr int ATLAS_FIRST_CHAR = 32; // space

static void build_font_atlas() {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_A8, ATLAS_W, ATLAS_H);
    cairo_t* cr = cairo_create(surface);

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* font = pango_font_description_from_string("Sans Bold 28");
    pango_layout_set_font_description(layout, font);

    cairo_set_source_rgba(cr, 1, 1, 1, 1);

    for (int i = 0; i < 95; i++) {
        char ch = static_cast<char>(ATLAS_FIRST_CHAR + i);
        char str[2] = {ch, 0};
        pango_layout_set_text(layout, str, 1);
        int pw, ph;
        pango_layout_get_pixel_size(layout, &pw, &ph);
        int col = i % ATLAS_COLS;
        int row = i / ATLAS_COLS;
        float ox = col * CELL_SIZE + (CELL_SIZE - pw) * 0.5f;
        float oy = row * CELL_SIZE + (CELL_SIZE - ph) * 0.5f;
        cairo_move_to(cr, ox, oy);
        pango_cairo_show_layout(cr, layout);
    }

    cairo_surface_flush(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);

    glGenTextures(1, &g_font_tex);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // Cairo A8 stride may differ from width
    int stride = cairo_image_surface_get_stride(surface);
    if (stride == ATLAS_W) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ATLAS_W, ATLAS_H, 0, GL_RED, GL_UNSIGNED_BYTE, data);
    } else {
        // Copy row by row to handle stride mismatch
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ATLAS_W, ATLAS_H, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        for (int y = 0; y < ATLAS_H; y++)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, ATLAS_W, 1, GL_RED, GL_UNSIGNED_BYTE, data + y * stride);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    pango_font_description_free(font);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

// Get UV coords for a character from the atlas
static void char_uvs(char ch, float& u0, float& v0, float& u1, float& v1) {
    int idx = static_cast<int>(ch) - ATLAS_FIRST_CHAR;
    if (idx < 0 || idx >= 95) idx = 0; // space for unknown
    int col = idx % ATLAS_COLS;
    int row = idx / ATLAS_COLS;
    u0 = static_cast<float>(col) / ATLAS_COLS;
    v0 = static_cast<float>(row) / ATLAS_ROWS;
    u1 = static_cast<float>(col + 1) / ATLAS_COLS;
    v1 = static_cast<float>(row + 1) / ATLAS_ROWS;
}

// Add a textured quad for a character in NDC (2D screen space)
static void add_screen_char(std::vector<float>& verts, float x, float y,
                            float w, float h, char ch) {
    float u0, v0, u1, v1;
    char_uvs(ch, u0, v0, u1, v1);
    // Two triangles: BL, BR, TR, BL, TR, TL
    verts.insert(verts.end(), {x, y-h, 0, u0, v1});
    verts.insert(verts.end(), {x+w, y-h, 0, u1, v1});
    verts.insert(verts.end(), {x+w, y, 0, u1, v0});
    verts.insert(verts.end(), {x, y-h, 0, u0, v1});
    verts.insert(verts.end(), {x+w, y, 0, u1, v0});
    verts.insert(verts.end(), {x, y, 0, u0, v0});
}

// Add a string of characters in NDC, returns x advance
static float add_screen_string(std::vector<float>& verts, float x, float y,
                               float char_w, float char_h, const std::string& str) {
    for (char ch : str) {
        add_screen_char(verts, x, y, char_w, char_h, ch);
        x += char_w * 0.7f; // tighter spacing
    }
    return x;
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
        char letter = 'a' + (7 - col); // 'a' at col 7 (+X = screen left)
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

        int n = static_cast<int>(gs.score_history.size());

        // Compute Y positions for each score point
        std::vector<float> score_y(n);
        for (int i = 0; i < n; i++)
            score_y[i] = std::max(gy0, std::min(gy1, gy0 + gh*0.5f + (gs.score_history[i]/max_s)*gh*0.45f));

        std::vector<float> gv;

        // White fill: from score line DOWN to graph bottom (white advantage area)
        int white_fill_start = 0;
        for (int i = 0; i < n-1; i++) {
            float t0 = float(i)/(n-1), t1 = float(i+1)/(n-1);
            float x0 = gx0+t0*gw, x1 = gx0+t1*gw;
            // Two triangles: score_line to bottom
            gv.insert(gv.end(), {x0,score_y[i],0, x1,score_y[i+1],0, x1,gy0,0});
            gv.insert(gv.end(), {x0,score_y[i],0, x1,gy0,0, x0,gy0,0});
        }
        int white_fill_count = static_cast<int>(gv.size()/3) - white_fill_start;

        // Black fill: from score line UP to graph top (black advantage area)
        int black_fill_start = static_cast<int>(gv.size()/3);
        for (int i = 0; i < n-1; i++) {
            float t0 = float(i)/(n-1), t1 = float(i+1)/(n-1);
            float x0 = gx0+t0*gw, x1 = gx0+t1*gw;
            gv.insert(gv.end(), {x0,gy1,0, x1,gy1,0, x1,score_y[i+1],0});
            gv.insert(gv.end(), {x0,gy1,0, x1,score_y[i+1],0, x0,score_y[i],0});
        }
        int black_fill_count = static_cast<int>(gv.size()/3) - black_fill_start;

        // Center line
        int zl_start = static_cast<int>(gv.size()/3);
        float zy = gy0 + gh*0.5f;
        gv.insert(gv.end(), {gx0,zy,0, gx1,zy,0});
        int zc = 2;

        // Score line
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

        // White area (below line)
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0.85f,0.85f,0.85f,0.8f);
        glDrawArrays(GL_TRIANGLES, white_fill_start, white_fill_count);

        // Black area (above line)
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0.12f,0.12f,0.12f,0.8f);
        glDrawArrays(GL_TRIANGLES, black_fill_start, black_fill_count);

        // Center line
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0.5f,0.5f,0.5f,0.4f);
        glLineWidth(1); glDrawArrays(GL_LINES, zl_start, zc);

        // Score line (green)
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

        // Win percentage text (using font atlas)
        float cur = gs.score_history.back();
        float wp_val = 1.0f/(1.0f+std::exp(-cur*0.5f));
        int wpct = static_cast<int>(std::round(wp_val*100)); int bpct = 100-wpct;

        char ps[32];
        std::snprintf(ps, sizeof(ps), "%d%%", wpct); std::string ws = ps;
        std::snprintf(ps, sizeof(ps), "%d%%", bpct); std::string bs = ps;

        std::vector<float> tv;
        float pch_w = 0.022f, pch_h = 0.032f;
        float pty = gy1 + 0.025f;

        // White percentage (left)
        add_screen_string(tv, gx0 + 0.01f, pty, pch_w, pch_h, ws);
        int white_verts = static_cast<int>(tv.size() / 5);

        // Black percentage (right-aligned)
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
            Mat4 id = mat4_identity();
            glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"), 1, GL_FALSE, id.m);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_font_tex);
            glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);

            // White text
            glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 1, 1, 1, 0.9f);
            glDrawArrays(GL_TRIANGLES, 0, white_verts);
            // Black text
            glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 0.7f, 0.7f, 0.7f, 0.9f);
            glDrawArrays(GL_TRIANGLES, white_verts, total_verts - white_verts);

            glBindVertexArray(0); glDeleteBuffers(1, &tvbo); glDeleteVertexArrays(1, &tvao);
        }
        glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
    }

    // --- Move list below graph ---
    if (gs.snapshots.size() > 1) {
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Graph bottom edge as reference
        float gx0 = 0.55f, gx1 = 0.95f;
        float graph_bottom = 0.55f;
        float ml_center = (gx0 + gx1) * 0.5f;

        float ch_w = 0.020f, ch_h = 0.030f;
        float line_h = 0.034f;
        float num_w = 0.055f;
        float col_gap = 0.01f;
        float half_row_w = 0.08f; // approx half-width of one move column

        int total_moves = static_cast<int>(gs.snapshots.size()) - 1;
        int total_full_moves = (total_moves + 1) / 2;
        int max_lines = 12;
        int first_move = 0;
        if (total_full_moves > max_lines)
            first_move = total_full_moves - max_lines;
        int visible_lines = std::min(total_full_moves - first_move, max_lines);

        // Start from graph bottom, grow downward
        float ml_top = graph_bottom - 0.015f;
        // Center the columns: num | white_col | gap | black_col
        float row_w = num_w + half_row_w * 2 + col_gap;
        float ml_x0 = ml_center - row_w * 0.5f;

        // Build move text in segments: normal verts, then highlighted verts
        struct MoveEntry {
            std::vector<float> verts;
            int snapshot_idx; // which snapshot this move corresponds to (1-based)
        };
        std::vector<MoveEntry> normal_entries;
        std::vector<MoveEntry> highlight_entries;

        float y = ml_top;
        for (int move_num = first_move; move_num < first_move + visible_lines; move_num++) {
            int white_snap = move_num * 2 + 1; // snapshot after white's move
            int black_snap = move_num * 2 + 2; // snapshot after black's move

            // Move number — always normal color
            std::string num_str = std::to_string(move_num + 1) + ".";
            MoveEntry num_entry; num_entry.snapshot_idx = -1;
            add_screen_string(num_entry.verts, ml_x0, y, ch_w, ch_h, num_str);
            normal_entries.push_back(num_entry);

            // White's move
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

            // Black's move
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

        // Merge normal verts
        std::vector<float> normal_verts, hl_verts;
        for (auto& e : normal_entries)
            normal_verts.insert(normal_verts.end(), e.verts.begin(), e.verts.end());
        for (auto& e : highlight_entries)
            hl_verts.insert(hl_verts.end(), e.verts.begin(), e.verts.end());

        // Combine for single upload
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

            // Normal moves in light gray
            if (normal_count > 0) {
                glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 0.80f, 0.80f, 0.80f, 0.9f);
                glDrawArrays(GL_TRIANGLES, 0, normal_count);
            }

            // Highlighted move in yellow
            if (hl_count > 0) {
                glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 1.0f, 0.9f, 0.2f, 1.0f);
                glDrawArrays(GL_TRIANGLES, normal_count, hl_count);
            }

            glBindVertexArray(0);
            glDeleteBuffers(1, &mvbo); glDeleteVertexArrays(1, &mvao);
        }

        glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
    }
}

// ===========================================================================
// Menu screen
// ===========================================================================
void menu_init_physics(std::vector<PhysicsPiece>& pieces) {
    pieces.clear();
    // Spawn 12 pieces at random positions above the scene
    PieceType types[12] = {KING, QUEEN, BISHOP, KNIGHT, ROOK, PAWN,
                           KING, QUEEN, BISHOP, KNIGHT, ROOK, PAWN};
    for (int i = 0; i < 12; i++) {
        PhysicsPiece p;
        p.type = types[i];
        // Spread across the area, varying heights
        float angle = static_cast<float>(i) / 12.0f * 6.28f;
        float radius = 2.0f + static_cast<float>(i % 5) * 0.8f;
        p.x = std::cos(angle) * radius;
        p.z = std::sin(angle) * radius;
        p.y = 3.0f + static_cast<float>(i) * 1.5f; // stagger heights
        p.vx = std::sin(angle * 3.7f) * 1.5f;
        p.vy = 0.0f;
        p.vz = std::cos(angle * 2.3f) * 1.5f;
        p.rot_x = static_cast<float>(i * 47 % 360);
        p.rot_y = static_cast<float>(i * 73 % 360);
        p.rot_z = static_cast<float>(i * 31 % 360);
        p.spin_x = (static_cast<float>(i % 3) - 1.0f) * 60.0f;
        p.spin_y = (static_cast<float>(i % 5) - 2.0f) * 40.0f;
        p.spin_z = (static_cast<float>(i % 4) - 1.5f) * 30.0f;
        p.scale = 0.35f + static_cast<float>(i % 3) * 0.1f;
        pieces.push_back(p);
    }
}

void menu_update_physics(std::vector<PhysicsPiece>& pieces, float dt) {
    const float gravity = -9.8f;
    const float floor_y = -2.0f;
    const float bounce = 0.6f;
    const float wall = 6.0f;
    const float damping = 0.998f;

    for (auto& p : pieces) {
        p.vy += gravity * dt;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.z += p.vz * dt;

        p.rot_x += p.spin_x * dt;
        p.rot_y += p.spin_y * dt;
        p.rot_z += p.spin_z * dt;

        // Floor bounce
        if (p.y < floor_y) {
            p.y = floor_y;
            p.vy = std::abs(p.vy) * bounce;
            p.spin_x *= 0.8f;
            p.spin_y *= 0.8f;
            p.spin_z *= 0.8f;
            // Add some random spin on bounce
            p.spin_x += (p.vx > 0 ? 1 : -1) * 20.0f;
        }

        // Wall bounce
        if (p.x < -wall) { p.x = -wall; p.vx = std::abs(p.vx) * bounce; }
        if (p.x >  wall) { p.x =  wall; p.vx = -std::abs(p.vx) * bounce; }
        if (p.z < -wall) { p.z = -wall; p.vz = std::abs(p.vz) * bounce; }
        if (p.z >  wall) { p.z =  wall; p.vz = -std::abs(p.vz) * bounce; }

        p.vx *= damping;
        p.vz *= damping;
    }
}

// Button layout in NDC
static const float BTN_W = 0.35f, BTN_H = 0.08f;
static const float BTN_X = -BTN_W * 0.5f; // centered
static const float BTN_START_Y = 0.05f;
static const float BTN_QUIT_Y = -0.12f;

int menu_hit_test(double mx, double my, int width, int height) {
    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float ndc_y = 1.0f - 2.0f * static_cast<float>(my) / height;

    // Start button
    if (ndc_x >= BTN_X && ndc_x <= BTN_X + BTN_W &&
        ndc_y >= BTN_START_Y - BTN_H && ndc_y <= BTN_START_Y)
        return 1;
    // Quit button
    if (ndc_x >= BTN_X && ndc_x <= BTN_X + BTN_W &&
        ndc_y >= BTN_QUIT_Y - BTN_H && ndc_y <= BTN_QUIT_Y)
        return 2;
    return 0;
}

void renderer_draw_menu(const std::vector<PhysicsPiece>& pieces,
                        int width, int height, float time,
                        int hover_button) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glViewport(0, 0, width, height);

    float aspect = static_cast<float>(width) / static_cast<float>(height);
    float deg2rad = static_cast<float>(M_PI) / 180.0f;

    // Slowly orbiting camera
    float cam_angle = time * 15.0f; // degrees per second
    float cam_pitch = 25.0f;
    float cam_dist = 12.0f;

    Mat4 view = mat4_multiply(
        mat4_translate(0, 0, -cam_dist),
        mat4_multiply(mat4_rotate_x(cam_pitch * deg2rad),
                      mat4_rotate_y(cam_angle * deg2rad)));
    Mat4 proj = mat4_perspective(45.0f * deg2rad, aspect, 0.1f, 100.0f);

    float cy = cam_dist * std::sin(-cam_pitch * deg2rad);
    float cxz = cam_dist * std::cos(-cam_pitch * deg2rad);
    float cx = cxz * std::sin(-cam_angle * deg2rad);
    float cz = cxz * std::cos(-cam_angle * deg2rad);
    float vp_arr[3] = {cx, cy, cz};

    // Draw pieces with PBR
    glUseProgram(g_program);
    glUniformMatrix4fv(glGetUniformLocation(g_program, "uView"), 1, GL_FALSE, view.m);
    glUniformMatrix4fv(glGetUniformLocation(g_program, "uProjection"), 1, GL_FALSE, proj.m);

    // No shadow map for menu
    Mat4 dummy_lsm = mat4_identity();
    glUniformMatrix4fv(glGetUniformLocation(g_program, "uLightSpaceMatrix"), 1, GL_FALSE, dummy_lsm.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_shadow_tex); // bind but won't produce shadows
    glUniform1i(glGetUniformLocation(g_program, "uShadowMap"), 0);
    glUniform3fv(glGetUniformLocation(g_program, "uViewPos"), 1, vp_arr);

    float lpos[12] = {0.4f,1,0.6f, -0.5f,0.8f,-0.4f, 0,0.5f,-1, 0,0.5f,1};
    float lcol[12] = {3.0f,2.8f,2.5f, 1.2f,1.3f,1.5f, 0.8f,0.7f,0.6f, 0.8f,0.7f,0.6f};
    glUniform3fv(glGetUniformLocation(g_program, "uLightPositions"), 4, lpos);
    glUniform3fv(glGetUniformLocation(g_program, "uLightColors"), 4, lcol);

    for (const auto& p : pieces) {
        float s = BASE_PIECE_SCALE * piece_scale[p.type] * p.scale / 0.35f;
        Mat4 rot = mat4_multiply(
            mat4_rotate_z(p.rot_z * deg2rad),
            mat4_multiply(mat4_rotate_y(p.rot_y * deg2rad),
                          mat4_rotate_x(p.rot_x * deg2rad)));
        Mat4 pm = mat4_multiply(
            mat4_translate(p.x, p.y, p.z),
            mat4_multiply(mat4_scale(s, s, s), rot));

        // Alternate white and black pieces
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

    // Title: "3D CHESS"
    float title_ch_w = 0.07f, title_ch_h = 0.10f;
    std::string title = "3D CHESS";
    float title_w = title.size() * title_ch_w * 0.7f;
    float title_x = -title_w * 0.5f;
    add_screen_string(ui_verts, title_x, 0.35f, title_ch_w, title_ch_h, title);
    int title_count = static_cast<int>(ui_verts.size() / 5);

    // Subtitle
    float sub_ch_w = 0.018f, sub_ch_h = 0.028f;
    std::string subtitle = "Play against Claude AI";
    float sub_w = subtitle.size() * sub_ch_w * 0.7f;
    add_screen_string(ui_verts, -sub_w * 0.5f, 0.22f, sub_ch_w, sub_ch_h, subtitle);
    int subtitle_end = static_cast<int>(ui_verts.size() / 5);

    // Button backgrounds (using highlight shader later)
    // For now, store button text vertex ranges
    // "Start Game" button text
    float btn_ch_w = 0.028f, btn_ch_h = 0.042f;
    std::string start_text = "Start Game";
    float start_tw = start_text.size() * btn_ch_w * 0.7f;
    add_screen_string(ui_verts, -start_tw * 0.5f, BTN_START_Y - 0.018f,
                      btn_ch_w, btn_ch_h, start_text);
    int start_end = static_cast<int>(ui_verts.size() / 5);

    // "Quit" button text
    std::string quit_text = "Quit";
    float quit_tw = quit_text.size() * btn_ch_w * 0.7f;
    add_screen_string(ui_verts, -quit_tw * 0.5f, BTN_QUIT_Y - 0.018f,
                      btn_ch_w, btn_ch_h, quit_text);
    int quit_end = static_cast<int>(ui_verts.size() / 5);

    // Upload text
    GLuint uvao, uvbo;
    glGenVertexArrays(1, &uvao); glGenBuffers(1, &uvbo);
    glBindVertexArray(uvao);
    glBindBuffer(GL_ARRAY_BUFFER, uvbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(ui_verts.size() * sizeof(float)),
                 ui_verts.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Draw button backgrounds first with highlight shader
    glUseProgram(g_highlight_program);
    glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"), 1, GL_FALSE, id.m);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);

    // Draw button bg quads
    {
        std::vector<float> btn_bg;
        auto add_btn_bg = [&](float bx, float by, float bw, float bh, bool hovered) {
            float a = hovered ? 0.5f : 0.3f;
            (void)a; // color set via uniform
            btn_bg.insert(btn_bg.end(), {bx,by-bh,0, bx+bw,by-bh,0, bx+bw,by,0,
                                          bx,by-bh,0, bx+bw,by,0, bx,by,0});
        };
        add_btn_bg(BTN_X, BTN_START_Y, BTN_W, BTN_H, hover_button == 1);
        add_btn_bg(BTN_X, BTN_QUIT_Y, BTN_W, BTN_H, hover_button == 2);

        GLuint bvao, bvbo;
        glGenVertexArrays(1, &bvao); glGenBuffers(1, &bvbo);
        glBindVertexArray(bvao);
        glBindBuffer(GL_ARRAY_BUFFER, bvbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(btn_bg.size() * sizeof(float)),
                     btn_bg.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);

        // Start button bg
        float sa = (hover_button == 1) ? 0.5f : 0.3f;
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0.2f,0.4f,0.8f, sa);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Quit button bg
        float qa = (hover_button == 2) ? 0.5f : 0.3f;
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0.6f,0.15f,0.15f, qa);
        glDrawArrays(GL_TRIANGLES, 6, 6);

        glBindVertexArray(0); glDeleteBuffers(1, &bvbo); glDeleteVertexArrays(1, &bvao);
    }

    // Draw text
    glUseProgram(g_text_program);
    glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"), 1, GL_FALSE, id.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);

    glBindVertexArray(uvao);

    // Title - gold
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 1.0f, 0.9f, 0.6f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, title_count);

    // Subtitle - light gray
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 0.7f, 0.7f, 0.7f, 0.8f);
    glDrawArrays(GL_TRIANGLES, title_count, subtitle_end - title_count);

    // Start text - white (brighter on hover)
    float si = (hover_button == 1) ? 1.0f : 0.85f;
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), si, si, si, 1.0f);
    glDrawArrays(GL_TRIANGLES, subtitle_end, start_end - subtitle_end);

    // Quit text
    float qi = (hover_button == 2) ? 1.0f : 0.85f;
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), qi, qi, qi, 1.0f);
    glDrawArrays(GL_TRIANGLES, start_end, quit_end - start_end);

    glBindVertexArray(0);
    glDeleteBuffers(1, &uvbo); glDeleteVertexArrays(1, &uvao);

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}
