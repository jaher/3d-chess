#include "board_renderer.h"
#include "chess_rules.h"
#include "linalg.h"
#include "shader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

// Shatter transition
static GLuint g_shatter_program = 0;
static GLuint g_shatter_vao = 0, g_shatter_vbo = 0;
static int g_shatter_vertex_count = 0;
static GLuint g_capture_tex = 0;
#ifdef __EMSCRIPTEN__
static GLuint g_capture_fbo = 0;
#endif
static int g_capture_w = 0, g_capture_h = 0;

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
#ifdef __EMSCRIPTEN__
    // Web build: stb_truetype-based atlas baker (same cell layout).
    build_font_atlas_stb(&g_font_tex, ATLAS_W, ATLAS_H);
#else
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
#endif
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

// Build shatter mesh: Voronoi-like cells.
// For each jittered cell center, compute its voronoi polygon via
// Sutherland-Hodgman clipping against each neighbor's perpendicular bisector,
// then fan-triangulate from the centroid.
// Vertex layout: centerNDC(2) localOffset(2) uv(2) seed(1) = 7 floats
struct SPoint { float x, y; };

static std::vector<SPoint> clip_poly(const std::vector<SPoint>& poly,
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

// Clip against the bisector between center C and neighbor N (inside = closer to C)
static std::vector<SPoint> clip_bisector(const std::vector<SPoint>& poly,
                                           SPoint c, SPoint n_pt) {
    // Midpoint M, normal = N - C
    SPoint mid = {(c.x + n_pt.x) * 0.5f, (c.y + n_pt.y) * 0.5f};
    float nx = n_pt.x - c.x, ny = n_pt.y - c.y;
    // Plane: nx*(p-M) <= 0  =>  nx*p.x + ny*p.y - (nx*mid.x + ny*mid.y) <= 0
    float d = -(nx*mid.x + ny*mid.y);
    return clip_poly(poly, nx, ny, d);
}

static void build_shatter_mesh() {
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
        float jx = j.x * 0.7f + 0.15f; // jitter within 15-85% of cell
        float jy = j.y * 0.7f + 0.15f;
        return {-1.0f + (col + jx) * cw, -1.0f + (row + jy) * ch};
    };

    std::vector<float> verts;

    for (int row = 0; row < ny; row++) {
        for (int col = 0; col < nx; col++) {
            SPoint center = cell_center(col, row);

            // Start with a big square encompassing the screen
            std::vector<SPoint> poly = {
                {center.x - 2.5f, center.y - 2.5f},
                {center.x + 2.5f, center.y - 2.5f},
                {center.x + 2.5f, center.y + 2.5f},
                {center.x - 2.5f, center.y + 2.5f},
            };

            // Clip against 24 neighbors in a 5x5 area (better voronoi approximation)
            for (int dr = -2; dr <= 2 && !poly.empty(); dr++) {
                for (int dc = -2; dc <= 2; dc++) {
                    if (dc == 0 && dr == 0) continue;
                    SPoint n_pt = cell_center(col + dc, row + dr);
                    poly = clip_bisector(poly, center, n_pt);
                    if (poly.empty()) break;
                }
            }

            // Clip against screen bounds [-1, 1]
            poly = clip_poly(poly,  1, 0, -1); // x <= 1
            poly = clip_poly(poly, -1, 0, -1); // x >= -1
            poly = clip_poly(poly,  0, 1, -1); // y <= 1
            poly = clip_poly(poly,  0,-1, -1); // y >= -1

            if (poly.size() < 3) continue;

            // Compute centroid
            SPoint centroid = {0, 0};
            for (auto& p : poly) { centroid.x += p.x; centroid.y += p.y; }
            centroid.x /= poly.size();
            centroid.y /= poly.size();

            float seed = hash1(col, row);

            // Fan triangulate from centroid
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

void renderer_capture_frame(int width, int height) {
    if (g_capture_tex == 0 || g_capture_w != width || g_capture_h != height) {
        if (g_capture_tex) glDeleteTextures(1, &g_capture_tex);
        glGenTextures(1, &g_capture_tex);
        glBindTexture(GL_TEXTURE_2D, g_capture_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g_capture_w = width;
        g_capture_h = height;

#ifdef __EMSCRIPTEN__
        // Single-sample FBO that the multisample backbuffer can be
        // resolved into via glBlitFramebuffer. See note below.
        if (!g_capture_fbo) glGenFramebuffers(1, &g_capture_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, g_capture_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, g_capture_tex, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif
    }

#ifdef __EMSCRIPTEN__
    // The default WebGL 2 backbuffer is created with antialias=true (the
    // Emscripten/SDL2 default), which makes it multisampled. The OpenGL
    // ES 3.0 spec leaves glCopyTexSubImage2D from a multisample read
    // framebuffer UNDEFINED — in practice it produces an all-zero
    // texture, which then makes the shatter shards render solid black.
    // Resolve the multisample buffer into our single-sample capture
    // FBO via glBlitFramebuffer instead.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_capture_fbo);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
#else
    glBindTexture(GL_TEXTURE_2D, g_capture_tex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
#endif
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

void renderer_init(StlModel loaded_models[PIECE_COUNT]) {
    g_program = create_program(vertex_shader_src, fragment_shader_src);
    g_highlight_program = create_program(highlight_vs_src, highlight_fs_src);
    g_shadow_program = create_program(shadow_vs_src, shadow_fs_src);
    g_text_program = create_program(text_vs_src, text_fs_src);
    g_shatter_program = create_program(shatter_vs_src, shatter_fs_src);
    build_shatter_mesh();

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
static const float EG_MENU_BTN_X = -0.12f;
static const float EG_MENU_BTN_Y = -0.015f;
static const float EG_MENU_BTN_W =  0.24f;
static const float EG_MENU_BTN_H =  0.07f;

bool endgame_menu_button_hit_test(double mx, double my, int width, int height) {
    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float ndc_y = 1.0f - 2.0f * static_cast<float>(my) / height;
    return ndc_x >= EG_MENU_BTN_X && ndc_x <= EG_MENU_BTN_X + EG_MENU_BTN_W &&
           ndc_y >= EG_MENU_BTN_Y - EG_MENU_BTN_H && ndc_y <= EG_MENU_BTN_Y;
}

void renderer_draw(GameState& gs, int width, int height,
                   float rot_x, float rot_y, float zoom,
                   bool human_plays_white,
                   bool endgame_menu_hover) {
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

        // Compute Y positions for each score point. Flip the axis when
        // the human plays black so the human's color (black) ends up at
        // the bottom of the graph and white at the top.
        float sign = human_plays_white ? 1.0f : -1.0f;

        std::vector<float> score_y(n);
        for (int i = 0; i < n; i++)
            score_y[i] = std::max(gy0, std::min(gy1,
                gy0 + gh*0.5f + sign * (gs.score_history[i]/max_s)*gh*0.45f));

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

        // Fill below/above the score line. The below fill is always
        // the human's own color (so playing white → light at bottom,
        // playing black → dark at bottom).
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

        // Center line
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0.5f,0.5f,0.5f,0.4f);
        glLineWidth(1); glDrawArrays(GL_LINES, zl_start, zc);

        // Score line: 50% gray — the midpoint between the white and
        // black fills below it, so it visually reads as "neither side".
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0.5f,0.5f,0.5f,0.9f);
        glLineWidth(2); glDrawArrays(GL_LINES, ls, lc);

        // Analysis dot
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

    // --- Game over overlay ---
    if (gs.game_over && !gs.game_result.empty()) {
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Semi-transparent dark backdrop
        glUseProgram(g_highlight_program);
        Mat4 id_go = mat4_identity();
        glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"), 1, GL_FALSE, id_go.m);
        glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
        glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0, 0, 0, 0.5f);
        {
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

        // Big text
        glUseProgram(g_text_program);
        glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"), 1, GL_FALSE, id_go.m);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_font_tex);
        glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);

        std::vector<float> go_verts;
        float go_cw = 0.045f, go_ch = 0.065f;
        float go_w = gs.game_result.size() * go_cw * 0.7f;
        // Result text pushed up a bit so the "Back to Menu" button
        // fits under it inside the backdrop.
        add_screen_string(go_verts, -go_w * 0.5f, 0.085f, go_cw, go_ch, gs.game_result);
        int go_count = static_cast<int>(go_verts.size() / 5);

        // "Back to Menu" button label
        float btn_cw = 0.028f, btn_ch = 0.042f;
        std::string btn_label = "Back to Menu";
        float btn_lw = btn_label.size() * btn_cw * 0.7f;
        add_screen_string(go_verts, -btn_lw * 0.5f,
                          EG_MENU_BTN_Y - 0.018f, btn_cw, btn_ch, btn_label);
        int btn_label_end = static_cast<int>(go_verts.size() / 5);
        int btn_label_count = btn_label_end - go_count;

        // Button background — drawn via highlight_program before the text.
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
                        br, bg, bb, 0.92f);
            float bv[] = {
                EG_MENU_BTN_X,                EG_MENU_BTN_Y - EG_MENU_BTN_H, 0,
                EG_MENU_BTN_X + EG_MENU_BTN_W, EG_MENU_BTN_Y - EG_MENU_BTN_H, 0,
                EG_MENU_BTN_X + EG_MENU_BTN_W, EG_MENU_BTN_Y,                 0,
                EG_MENU_BTN_X,                EG_MENU_BTN_Y - EG_MENU_BTN_H, 0,
                EG_MENU_BTN_X + EG_MENU_BTN_W, EG_MENU_BTN_Y,                 0,
                EG_MENU_BTN_X,                EG_MENU_BTN_Y,                 0,
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

            // Rebind the text program for the result + button label draws.
            glUseProgram(g_text_program);
            glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"),
                               1, GL_FALSE, id_go.m);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_font_tex);
            glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);
        }

        if (go_count > 0 || btn_label_count > 0) {
            GLuint gvao, gvbo;
            glGenVertexArrays(1, &gvao); glGenBuffers(1, &gvbo);
            glBindVertexArray(gvao); glBindBuffer(GL_ARRAY_BUFFER, gvbo);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(go_verts.size()*sizeof(float)),
                         go_verts.data(), GL_STREAM_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)0);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)(3*sizeof(float)));
            glEnableVertexAttribArray(1);

            // Gold game result text
            glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 1.0f, 0.9f, 0.5f, 1.0f);
            glDrawArrays(GL_TRIANGLES, 0, go_count);

            // White button label (brighter on hover)
            float lb = endgame_menu_hover ? 1.0f : 0.92f;
            glUniform4f(glGetUniformLocation(g_text_program, "uColor"), lb, lb, lb, 1.0f);
            glDrawArrays(GL_TRIANGLES, go_count, btn_label_count);

            glBindVertexArray(0); glDeleteBuffers(1, &gvbo); glDeleteVertexArrays(1, &gvao);
        }

        glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
    }
}

// ===========================================================================
// Menu screen
// ===========================================================================
void menu_init_physics(std::vector<PhysicsPiece>& pieces) {
    pieces.clear();
    PieceType types[12] = {KING,QUEEN,BISHOP,KNIGHT,ROOK,PAWN,KING,QUEEN,BISHOP,KNIGHT,ROOK,PAWN};
    for (int i = 0; i < 12; i++) {
        PhysicsPiece p;
        p.type = types[i];
        float angle = static_cast<float>(i) / 12.0f * 6.28f;
        float radius = 2.0f + static_cast<float>(i % 5) * 0.8f;
        p.x = std::cos(angle) * radius;
        p.z = std::sin(angle) * radius;
        p.y = 3.0f + static_cast<float>(i) * 1.5f;
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
    const float piece_radius = 0.5f;

    for (auto& p : pieces) {
        p.vy += gravity * dt;
        p.x += p.vx * dt; p.y += p.vy * dt; p.z += p.vz * dt;
        p.rot_x += p.spin_x * dt; p.rot_y += p.spin_y * dt; p.rot_z += p.spin_z * dt;

        if (p.y < floor_y) {
            p.y = floor_y; p.vy = std::abs(p.vy) * bounce;
            p.spin_x *= 0.8f; p.spin_y *= 0.8f; p.spin_z *= 0.8f;
            p.spin_x += (p.vx > 0 ? 1 : -1) * 20.0f;
        }
        if (p.x < -wall) { p.x = -wall; p.vx = std::abs(p.vx) * bounce; }
        if (p.x >  wall) { p.x =  wall; p.vx = -std::abs(p.vx) * bounce; }
        if (p.z < -wall) { p.z = -wall; p.vz = std::abs(p.vz) * bounce; }
        if (p.z >  wall) { p.z =  wall; p.vz = -std::abs(p.vz) * bounce; }
        p.vx *= damping; p.vz *= damping;
    }

    // Piece-piece collisions
    int n = static_cast<int>(pieces.size());
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            auto& a = pieces[i]; auto& b = pieces[j];
            float dx = b.x-a.x, dy = b.y-a.y, dz = b.z-a.z;
            float dist2 = dx*dx+dy*dy+dz*dz;
            float md = piece_radius * (a.scale+b.scale) / 0.35f;
            if (dist2 < md*md && dist2 > 0.001f) {
                float dist = std::sqrt(dist2);
                float nx=dx/dist, ny=dy/dist, nz=dz/dist;
                float ov = md-dist;
                a.x-=nx*ov*0.5f; a.y-=ny*ov*0.5f; a.z-=nz*ov*0.5f;
                b.x+=nx*ov*0.5f; b.y+=ny*ov*0.5f; b.z+=nz*ov*0.5f;
                float rv=(a.vx-b.vx)*nx+(a.vy-b.vy)*ny+(a.vz-b.vz)*nz;
                if (rv > 0) {
                    float imp=rv*0.85f;
                    a.vx-=imp*nx; a.vy-=imp*ny; a.vz-=imp*nz;
                    b.vx+=imp*nx; b.vy+=imp*ny; b.vz+=imp*nz;
                }
            }
        }
    }
}

static const float BTN_W = 0.35f, BTN_H = 0.08f;
static const float BTN_X = -BTN_W * 0.5f;
static const float BTN_START_Y = 0.12f;
static const float BTN_CHALLENGE_Y = -0.05f;
static const float BTN_QUIT_Y = -0.22f;

int menu_hit_test(double mx, double my, int width, int height) {
    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float ndc_y = 1.0f - 2.0f * static_cast<float>(my) / height;
    if (ndc_x >= BTN_X && ndc_x <= BTN_X + BTN_W && ndc_y >= BTN_START_Y - BTN_H && ndc_y <= BTN_START_Y) return 1;
    if (ndc_x >= BTN_X && ndc_x <= BTN_X + BTN_W && ndc_y >= BTN_CHALLENGE_Y - BTN_H && ndc_y <= BTN_CHALLENGE_Y) return 3;
#ifndef __EMSCRIPTEN__
    // No "Quit" button in the browser build — closing a tab from inside
    // a WASM app is awkward and not expected on the web.
    if (ndc_x >= BTN_X && ndc_x <= BTN_X + BTN_W && ndc_y >= BTN_QUIT_Y - BTN_H && ndc_y <= BTN_QUIT_Y) return 2;
#endif
    return 0;
}

void renderer_draw_menu(const std::vector<PhysicsPiece>& pieces,
                        int width, int height, float time,
                        int hover_button) {
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

    float scw = 0.018f, sch = 0.028f;
    std::string subtitle = "Play against stockfish";
    float sw = subtitle.size() * scw * 0.7f;
    add_screen_string(ui_verts, -sw*0.5f, 0.22f, scw, sch, subtitle);
    int subtitle_end = static_cast<int>(ui_verts.size() / 5);

    float bcw = 0.028f, bch = 0.042f;
    std::string start_text = "Start Game";
    float stw = start_text.size() * bcw * 0.7f;
    add_screen_string(ui_verts, -stw*0.5f, BTN_START_Y - 0.018f, bcw, bch, start_text);
    int start_end = static_cast<int>(ui_verts.size() / 5);

    std::string ch_text = "Challenges";
    float chw = ch_text.size() * bcw * 0.7f;
    add_screen_string(ui_verts, -chw*0.5f, BTN_CHALLENGE_Y - 0.018f, bcw, bch, ch_text);
    int ch_end = static_cast<int>(ui_verts.size() / 5);

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
        float bg[] = {
            BTN_X,BTN_START_Y-BTN_H,0, BTN_X+BTN_W,BTN_START_Y-BTN_H,0, BTN_X+BTN_W,BTN_START_Y,0,
            BTN_X,BTN_START_Y-BTN_H,0, BTN_X+BTN_W,BTN_START_Y,0, BTN_X,BTN_START_Y,0,
            BTN_X,BTN_CHALLENGE_Y-BTN_H,0, BTN_X+BTN_W,BTN_CHALLENGE_Y-BTN_H,0, BTN_X+BTN_W,BTN_CHALLENGE_Y,0,
            BTN_X,BTN_CHALLENGE_Y-BTN_H,0, BTN_X+BTN_W,BTN_CHALLENGE_Y,0, BTN_X,BTN_CHALLENGE_Y,0,
            BTN_X,BTN_QUIT_Y-BTN_H,0, BTN_X+BTN_W,BTN_QUIT_Y-BTN_H,0, BTN_X+BTN_W,BTN_QUIT_Y,0,
            BTN_X,BTN_QUIT_Y-BTN_H,0, BTN_X+BTN_W,BTN_QUIT_Y,0, BTN_X,BTN_QUIT_Y,0
        };
        GLuint bvao, bvbo;
        glGenVertexArrays(1, &bvao); glGenBuffers(1, &bvbo);
        glBindVertexArray(bvao); glBindBuffer(GL_ARRAY_BUFFER, bvbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(bg), bg, GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0.2f,0.4f,0.8f, hover_button==1?0.5f:0.3f);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0.2f,0.6f,0.3f, hover_button==3?0.5f:0.3f);
        glDrawArrays(GL_TRIANGLES, 6, 6);
#ifndef __EMSCRIPTEN__
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0.6f,0.15f,0.15f, hover_button==2?0.5f:0.3f);
        glDrawArrays(GL_TRIANGLES, 12, 6);
#endif
        glBindVertexArray(0); glDeleteBuffers(1, &bvbo); glDeleteVertexArrays(1, &bvao);
    }

    glUseProgram(g_text_program);
    glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"), 1, GL_FALSE, id.m);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);
    glBindVertexArray(uvao);

    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 1,0.9f,0.6f,1);
    glDrawArrays(GL_TRIANGLES, 0, title_count);
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 0.7f,0.7f,0.7f,0.8f);
    glDrawArrays(GL_TRIANGLES, title_count, subtitle_end - title_count);
    float si = hover_button==1 ? 1.0f : 0.85f;
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), si,si,si,1);
    glDrawArrays(GL_TRIANGLES, subtitle_end, start_end - subtitle_end);
    float ci = hover_button==3 ? 1.0f : 0.85f;
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), ci,ci,ci,1);
    glDrawArrays(GL_TRIANGLES, start_end, ch_end - start_end);
#ifndef __EMSCRIPTEN__
    float qi = hover_button==2 ? 1.0f : 0.85f;
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), qi,qi,qi,1);
    glDrawArrays(GL_TRIANGLES, ch_end, quit_end - ch_end);
#endif

    glBindVertexArray(0); glDeleteBuffers(1, &uvbo); glDeleteVertexArrays(1, &uvao);
    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}

// ===========================================================================
// Pre-game setup screen (side toggle + Stockfish ELO slider)
// ===========================================================================
// NDC layout:
//
//   y = +0.55   "Game Setup"            (title)
//   y = +0.25   [ White/Black moves first ] (toggle button)
//   y = -0.05   "Stockfish strength — NNNN"
//   y = -0.25   gradient slider bar (width 1.20, height 0.06)
//                                      + handle below it
//   y = -0.60   [     Start      ]
//   top-left    [ Back ]
//
// The slider's horizontal span MUST match APP_SLIDER_NDC_LEFT/RIGHT in
// app_state.cpp so the pixel-to-ELO math in slider_px_to_elo lines up
// with what's rendered here.
static const float PG_TOGGLE_W = 0.60f;
static const float PG_TOGGLE_H = 0.10f;
static const float PG_TOGGLE_X = -PG_TOGGLE_W * 0.5f;
static const float PG_TOGGLE_Y =  0.25f;

static const float PG_SLIDER_X_LEFT  = -0.60f;  // mirrors APP_SLIDER_NDC_LEFT
static const float PG_SLIDER_X_RIGHT = +0.60f;  // mirrors APP_SLIDER_NDC_RIGHT
static const float PG_SLIDER_Y       = -0.22f;
static const float PG_SLIDER_H       =  0.07f;   // pill height (rounded caps
                                                 // have radius = H/2).
static const float PG_SLIDER_STROKE  =  0.006f;  // outline thickness (NDC)

static const float PG_START_W = 0.40f;
static const float PG_START_H = 0.12f;
static const float PG_START_X = -PG_START_W * 0.5f;
static const float PG_START_Y = -0.55f;

// Back button reuses the challenge-select back-button NDC rectangle
// (CS_BACK_* below).

int pregame_hit_test(double mx, double my, int width, int height) {
    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float ndc_y = 1.0f - 2.0f * static_cast<float>(my) / height;

    // Back button (same rect as challenge-select back)
    static const float BACK_X = -0.95f, BACK_Y = 0.93f;
    static const float BACK_W =  0.20f, BACK_H = 0.07f;
    if (ndc_x >= BACK_X && ndc_x <= BACK_X + BACK_W &&
        ndc_y >= BACK_Y - BACK_H && ndc_y <= BACK_Y)
        return 2;

    // Start
    if (ndc_x >= PG_START_X && ndc_x <= PG_START_X + PG_START_W &&
        ndc_y >= PG_START_Y - PG_START_H && ndc_y <= PG_START_Y)
        return 1;

    // Toggle
    if (ndc_x >= PG_TOGGLE_X && ndc_x <= PG_TOGGLE_X + PG_TOGGLE_W &&
        ndc_y >= PG_TOGGLE_Y - PG_TOGGLE_H && ndc_y <= PG_TOGGLE_Y)
        return 3;

    // Slider — widen the hit area a bit so clicks near the edges still
    // register. The visible bar spans y from (PG_SLIDER_Y) to
    // (PG_SLIDER_Y - PG_SLIDER_H); the hit box pads that by 0.02 NDC.
    float slider_hit_top    = PG_SLIDER_Y + 0.02f;
    float slider_hit_bottom = PG_SLIDER_Y - PG_SLIDER_H - 0.02f;
    if (ndc_x >= PG_SLIDER_X_LEFT - 0.03f && ndc_x <= PG_SLIDER_X_RIGHT + 0.03f &&
        ndc_y <= slider_hit_top && ndc_y >= slider_hit_bottom)
        return 4;

    return 0;
}

void renderer_draw_pregame(bool human_plays_white,
                           int elo, int elo_min, int elo_max,
                           int width, int height, int hover) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glViewport(0, 0, width, height);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Mat4 id = mat4_identity();

    // ----- Flat-color geometry (buttons + slider + handle) -----
    std::vector<float> bg_verts;  // 3 floats per vertex
    // Per-quad color regions so we can re-bind uColor between draws.
    struct Region { int start_vert, count; float r, g, b, a; };
    std::vector<Region> regions;

    auto add_quad = [&](float x, float y, float w, float h,
                        float r, float g, float b, float a) {
        int start = static_cast<int>(bg_verts.size() / 3);
        bg_verts.insert(bg_verts.end(),
            {x,   y-h, 0,  x+w, y-h, 0,  x+w, y, 0,
             x,   y-h, 0,  x+w, y, 0,    x,   y, 0});
        regions.push_back({start, 6, r, g, b, a});
    };

    // Back button background
    add_quad(-0.95f, 0.93f, 0.20f, 0.07f,
             0.25f, 0.35f, 0.55f, hover == 2 ? 0.55f : 0.30f);

    // Toggle button background (blue for white-first, dark for black-first)
    if (human_plays_white) {
        add_quad(PG_TOGGLE_X, PG_TOGGLE_Y, PG_TOGGLE_W, PG_TOGGLE_H,
                 0.85f, 0.82f, 0.75f, hover == 3 ? 0.75f : 0.55f);
    } else {
        add_quad(PG_TOGGLE_X, PG_TOGGLE_Y, PG_TOGGLE_W, PG_TOGGLE_H,
                 0.12f, 0.12f, 0.14f, hover == 3 ? 0.95f : 0.78f);
    }

    // Start button background
    add_quad(PG_START_X, PG_START_Y, PG_START_W, PG_START_H,
             0.20f, 0.60f, 0.30f, hover == 1 ? 0.75f : 0.55f);

    // ----- Upload + draw all flat-colored geometry -----
    GLuint bvao, bvbo;
    glGenVertexArrays(1, &bvao); glGenBuffers(1, &bvbo);
    glBindVertexArray(bvao);
    glBindBuffer(GL_ARRAY_BUFFER, bvbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(bg_verts.size() * sizeof(float)),
                 bg_verts.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glUseProgram(g_highlight_program);
    glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"),
                       1, GL_FALSE, id.m);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
    // Flat-color mode for the buttons + handle.
    glUniform1i(glGetUniformLocation(g_highlight_program, "uUseGradient"), 0);
    for (const auto& r : regions) {
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    r.r, r.g, r.b, r.a);
        glDrawArrays(GL_TRIANGLES, r.start_vert, r.count);
    }
    glBindVertexArray(0);
    glDeleteBuffers(1, &bvbo); glDeleteVertexArrays(1, &bvao);

    // ----- Pill-shaped progress slider -----
    //
    // Three layered draws form the capsule with an outlined stroke and
    // an interior gradient "fill" that slides from left to right as
    // the ELO value increases:
    //
    //   1. Outer pill — a slightly larger capsule in the stroke color.
    //   2. Inner pill — the same capsule inset by PG_SLIDER_STROKE in
    //      the (dark) background color. Only the thin strip between
    //      outer and inner remains visible as the stroke.
    //   3. Gradient fill — a partial capsule from x_left up to fill_x,
    //      painted with the green->red gradient shader mode. The
    //      right edge of the fill follows the capsule boundary
    //      (curving along the right cap when the fill is near the
    //      right end), which is implemented by iterating narrow
    //      vertical slices whose top/bottom y follow half_h(x).
    //
    // The handle (square + triangle) is gone — the leading edge of
    // the gradient fill is the position indicator.
    {
        // Slice-based pill mesh builder. Emits triangles into a vec3
        // buffer for x in [x_from, x_to] sampled at N slices, with
        // top/bottom y values computed from half_h() given the
        // capsule (cap_l_cx, cap_r_cx, R) it belongs to.
        auto emit_pill_slices = [](std::vector<float>& out,
                                   float x_from, float x_to,
                                   float y_mid,
                                   float cap_l_cx, float cap_r_cx,
                                   float R, int slices) {
            if (x_to <= x_from) return;
            auto half_h = [&](float x) -> float {
                if (x <= cap_l_cx) {
                    float dx = cap_l_cx - x;
                    if (dx >= R) return 0;
                    return std::sqrt(R*R - dx*dx);
                }
                if (x >= cap_r_cx) {
                    float dx = x - cap_r_cx;
                    if (dx >= R) return 0;
                    return std::sqrt(R*R - dx*dx);
                }
                return R;
            };
            for (int i = 0; i < slices; i++) {
                float x0 = x_from + static_cast<float>(i)     / slices * (x_to - x_from);
                float x1 = x_from + static_cast<float>(i + 1) / slices * (x_to - x_from);
                float h0 = half_h(x0);
                float h1 = half_h(x1);
                // Quad (x0,y_mid-h0)-(x1,y_mid-h1)-(x1,y_mid+h1)-(x0,y_mid+h0).
                out.insert(out.end(),
                    {x0, y_mid - h0, 0,
                     x1, y_mid - h1, 0,
                     x1, y_mid + h1, 0,
                     x0, y_mid - h0, 0,
                     x1, y_mid + h1, 0,
                     x0, y_mid + h0, 0});
            }
        };

        // Inner (fill) capsule.
        const float R_in     = PG_SLIDER_H * 0.5f;
        const float y_mid_in = PG_SLIDER_Y - R_in;
        const float cap_l_in = PG_SLIDER_X_LEFT  + R_in;
        const float cap_r_in = PG_SLIDER_X_RIGHT - R_in;

        // Outer capsule is inflated in all four directions by
        // PG_SLIDER_STROKE, which also grows its radius by the same.
        const float R_out     = R_in + PG_SLIDER_STROKE;
        const float y_mid_out = y_mid_in;
        const float x_left_out  = PG_SLIDER_X_LEFT  - PG_SLIDER_STROKE;
        const float x_right_out = PG_SLIDER_X_RIGHT + PG_SLIDER_STROKE;
        const float cap_l_out = x_left_out  + R_out;  // = cap_l_in
        const float cap_r_out = x_right_out - R_out;  // = cap_r_in

        const int PILL_SLICES = 96;

        // Build outer + inner meshes into one big flat-color VBO.
        std::vector<float> pill_verts;
        int outer_start = 0;
        emit_pill_slices(pill_verts,
            x_left_out, x_right_out,
            y_mid_out, cap_l_out, cap_r_out, R_out, PILL_SLICES);
        int outer_count = static_cast<int>(pill_verts.size() / 3) - outer_start;

        int inner_start = static_cast<int>(pill_verts.size() / 3);
        emit_pill_slices(pill_verts,
            PG_SLIDER_X_LEFT, PG_SLIDER_X_RIGHT,
            y_mid_in, cap_l_in, cap_r_in, R_in, PILL_SLICES);
        int inner_count = static_cast<int>(pill_verts.size() / 3) - inner_start;

        // Gradient fill: partial inner capsule from left edge up to
        // fill_x. t clamps the ELO to the slider's range.
        float t = static_cast<float>(elo - elo_min) /
                  static_cast<float>(elo_max - elo_min);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        float fill_x = PG_SLIDER_X_LEFT +
                       t * (PG_SLIDER_X_RIGHT - PG_SLIDER_X_LEFT);

        int fill_start = static_cast<int>(pill_verts.size() / 3);
        if (fill_x > PG_SLIDER_X_LEFT) {
            // Use the same slice count scaled by the fill fraction so
            // each slice is roughly the same width as the full pill's
            // slices — keeps the right edge smooth regardless of t.
            int fill_slices = std::max(4,
                static_cast<int>(std::ceil(PILL_SLICES * t)));
            emit_pill_slices(pill_verts,
                PG_SLIDER_X_LEFT, fill_x,
                y_mid_in, cap_l_in, cap_r_in, R_in, fill_slices);
        }
        int fill_count = static_cast<int>(pill_verts.size() / 3) - fill_start;

        // Upload.
        GLuint pvao, pvbo;
        glGenVertexArrays(1, &pvao); glGenBuffers(1, &pvbo);
        glBindVertexArray(pvao);
        glBindBuffer(GL_ARRAY_BUFFER, pvbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(pill_verts.size() * sizeof(float)),
                     pill_verts.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);

        // Shader is already bound as highlight_program, inner/outer
        // radius uniforms are 0, uMVP is identity.

        // (1) Outer pill: flat color, the stroke.
        glUniform1i(glGetUniformLocation(g_highlight_program, "uUseGradient"), 0);
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    0.72f, 0.76f, 0.90f, 1.0f);  // cool off-white stroke
        glDrawArrays(GL_TRIANGLES, outer_start, outer_count);

        // (2) Inner pill: flat dark background, covers all but the stroke.
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    0.06f, 0.06f, 0.08f, 1.0f);  // near-black interior
        glDrawArrays(GL_TRIANGLES, inner_start, inner_count);

        // (3) Gradient fill: partial inner pill from left up to fill_x.
        if (fill_count > 0) {
            glUniform1i(glGetUniformLocation(g_highlight_program, "uUseGradient"), 1);
            glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                        0.20f, 0.85f, 0.25f, 1.0f);   // green (left)
            glUniform4f(glGetUniformLocation(g_highlight_program, "uColorB"),
                        0.92f, 0.20f, 0.18f, 1.0f);   // red   (right)
            glUniform1f(glGetUniformLocation(g_highlight_program, "uGradX0"),
                        PG_SLIDER_X_LEFT);
            glUniform1f(glGetUniformLocation(g_highlight_program, "uGradX1"),
                        PG_SLIDER_X_RIGHT);
            glDrawArrays(GL_TRIANGLES, fill_start, fill_count);
        }

        // Reset gradient mode so later draws in this frame (or in the
        // next renderer pass) see a flat-color highlight program.
        glUniform1i(glGetUniformLocation(g_highlight_program, "uUseGradient"), 0);

        glBindVertexArray(0);
        glDeleteBuffers(1, &pvbo); glDeleteVertexArrays(1, &pvao);
    }

    // ----- Text -----
    std::vector<float> ui_verts;  // 5 floats per vertex (xyz uv)

    // Title
    float tcw = 0.07f, tch = 0.10f;
    std::string title = "Game Setup";
    float tw = title.size() * tcw * 0.7f;
    add_screen_string(ui_verts, -tw * 0.5f, 0.58f, tcw, tch, title);
    int title_count = static_cast<int>(ui_verts.size() / 5);

    // Toggle button label
    float bcw = 0.028f, bch = 0.042f;
    const char* toggle_text_c =
        human_plays_white ? "White moves first" : "Black moves first";
    std::string toggle_text = toggle_text_c;
    float gw = toggle_text.size() * bcw * 0.7f;
    add_screen_string(ui_verts, -gw * 0.5f,
                      PG_TOGGLE_Y - 0.025f, bcw, bch, toggle_text);
    int toggle_end = static_cast<int>(ui_verts.size() / 5);

    // ELO label + current value
    float scw = 0.025f, sch = 0.038f;
    char elo_buf[64];
    std::snprintf(elo_buf, sizeof(elo_buf), "Stockfish strength  %d", elo);
    std::string elo_label = elo_buf;
    float ew = elo_label.size() * scw * 0.7f;
    add_screen_string(ui_verts, -ew * 0.5f, -0.04f, scw, sch, elo_label);
    int elo_end = static_cast<int>(ui_verts.size() / 5);

    // Start button label
    std::string start_text = "Start";
    float stw = start_text.size() * bcw * 0.7f;
    add_screen_string(ui_verts, -stw * 0.5f,
                      PG_START_Y - 0.035f, bcw, bch, start_text);
    int start_end = static_cast<int>(ui_verts.size() / 5);

    // Back button label
    float bkw = 0.020f, bkh = 0.030f;
    std::string back_text = "Back";
    add_screen_string(ui_verts, -0.92f, 0.91f, bkw, bkh, back_text);
    int back_end = static_cast<int>(ui_verts.size() / 5);

    // Upload + draw the text geometry
    GLuint uvao, uvbo;
    glGenVertexArrays(1, &uvao); glGenBuffers(1, &uvbo);
    glBindVertexArray(uvao);
    glBindBuffer(GL_ARRAY_BUFFER, uvbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(ui_verts.size() * sizeof(float)),
                 ui_verts.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glUseProgram(g_text_program);
    glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"),
                       1, GL_FALSE, id.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);

    // Title — warm highlight
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                1.0f, 0.9f, 0.6f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, title_count);

    // Toggle label — the text color contrasts with the button bg.
    if (human_plays_white) {
        glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                    0.10f, 0.10f, 0.10f, 1.0f);
    } else {
        glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                    0.95f, 0.95f, 0.95f, 1.0f);
    }
    glDrawArrays(GL_TRIANGLES, title_count, toggle_end - title_count);

    // ELO label — subtle white
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                0.85f, 0.85f, 0.85f, 1.0f);
    glDrawArrays(GL_TRIANGLES, toggle_end, elo_end - toggle_end);

    // Start label — bright white when hovered
    {
        float b = hover == 1 ? 1.0f : 0.92f;
        glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                    b, b, b, 1.0f);
        glDrawArrays(GL_TRIANGLES, elo_end, start_end - elo_end);
    }

    // Back label
    {
        float b = hover == 2 ? 1.0f : 0.85f;
        glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                    b, b, b, 1.0f);
        glDrawArrays(GL_TRIANGLES, start_end, back_end - start_end);
    }

    glBindVertexArray(0);
    glDeleteBuffers(1, &uvbo); glDeleteVertexArrays(1, &uvao);

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}

// ===========================================================================
// Challenge select screen
// ===========================================================================
static const float CS_BTN_W = 0.6f;
static const float CS_BTN_H = 0.08f;
static const float CS_BTN_X = -CS_BTN_W * 0.5f;
static const float CS_TOP_Y = 0.4f;
static const float CS_GAP = 0.02f;
static const float CS_BACK_W = 0.2f;
static const float CS_BACK_H = 0.07f;
static const float CS_BACK_X = -0.95f;
static const float CS_BACK_Y = 0.93f;

int challenge_select_hit_test(double mx, double my, int width, int height, int num_challenges) {
    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float ndc_y = 1.0f - 2.0f * static_cast<float>(my) / height;

    // Back button
    if (ndc_x >= CS_BACK_X && ndc_x <= CS_BACK_X + CS_BACK_W &&
        ndc_y >= CS_BACK_Y - CS_BACK_H && ndc_y <= CS_BACK_Y)
        return -2;

    // Challenge buttons
    for (int i = 0; i < num_challenges; i++) {
        float by = CS_TOP_Y - i * (CS_BTN_H + CS_GAP);
        if (ndc_x >= CS_BTN_X && ndc_x <= CS_BTN_X + CS_BTN_W &&
            ndc_y >= by - CS_BTN_H && ndc_y <= by)
            return i;
    }
    return -1;
}

void renderer_draw_challenge_select(const std::vector<std::string>& challenge_names,
                                    int width, int height, int hover_index) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glViewport(0, 0, width, height);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Mat4 id = mat4_identity();

    // --- Build button background quads ---
    std::vector<float> bg_verts;
    auto add_quad = [&](float x, float y, float w, float h) {
        bg_verts.insert(bg_verts.end(), {x, y-h, 0,  x+w, y-h, 0,  x+w, y, 0,
                                          x, y-h, 0,  x+w, y, 0,  x, y, 0});
    };
    add_quad(CS_BACK_X, CS_BACK_Y, CS_BACK_W, CS_BACK_H); // back button
    for (int i = 0; i < static_cast<int>(challenge_names.size()); i++) {
        float by = CS_TOP_Y - i * (CS_BTN_H + CS_GAP);
        add_quad(CS_BTN_X, by, CS_BTN_W, CS_BTN_H);
    }

    GLuint bvao, bvbo;
    glGenVertexArrays(1, &bvao); glGenBuffers(1, &bvbo);
    glBindVertexArray(bvao); glBindBuffer(GL_ARRAY_BUFFER, bvbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bg_verts.size()*sizeof(float)),
                 bg_verts.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glUseProgram(g_highlight_program);
    glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"), 1, GL_FALSE, id.m);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);

    // Back button
    glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                0.4f, 0.4f, 0.4f, hover_index == -2 ? 0.6f : 0.4f);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Challenge buttons
    for (int i = 0; i < static_cast<int>(challenge_names.size()); i++) {
        bool h = (hover_index == i);
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    0.2f, 0.6f, 0.3f, h ? 0.6f : 0.35f);
        glDrawArrays(GL_TRIANGLES, 6 + i*6, 6);
    }
    glBindVertexArray(0); glDeleteBuffers(1, &bvbo); glDeleteVertexArrays(1, &bvao);

    // --- Text ---
    std::vector<float> text_verts;

    // Title
    float tcw = 0.05f, tch = 0.075f;
    std::string title = "Select Challenge";
    float tw = title.size() * tcw * 0.7f;
    add_screen_string(text_verts, -tw*0.5f, 0.6f, tcw, tch, title);
    int title_count = static_cast<int>(text_verts.size() / 5);

    // Back button text
    float bw_cw = 0.022f, bw_ch = 0.032f;
    std::string back_text = "< Back";
    add_screen_string(text_verts, CS_BACK_X + 0.04f, CS_BACK_Y - 0.020f, bw_cw, bw_ch, back_text);
    int back_end = static_cast<int>(text_verts.size() / 5);

    // Challenge names
    float cw = 0.024f, ch = 0.036f;
    std::vector<int> name_ends;
    for (int i = 0; i < static_cast<int>(challenge_names.size()); i++) {
        float by = CS_TOP_Y - i * (CS_BTN_H + CS_GAP);
        float nw = challenge_names[i].size() * cw * 0.7f;
        add_screen_string(text_verts, -nw*0.5f, by - 0.025f, cw, ch, challenge_names[i]);
        name_ends.push_back(static_cast<int>(text_verts.size() / 5));
    }

    if (text_verts.empty()) {
        glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
        return;
    }

    GLuint tvao, tvbo;
    glGenVertexArrays(1, &tvao); glGenBuffers(1, &tvbo);
    glBindVertexArray(tvao); glBindBuffer(GL_ARRAY_BUFFER, tvbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(text_verts.size()*sizeof(float)),
                 text_verts.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(1);

    glUseProgram(g_text_program);
    glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"), 1, GL_FALSE, id.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);

    // Title
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 1.0f, 0.9f, 0.6f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, title_count);

    // Back text
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 0.85f, 0.85f, 0.85f, 1.0f);
    glDrawArrays(GL_TRIANGLES, title_count, back_end - title_count);

    // Challenge name texts
    int prev = back_end;
    for (int i = 0; i < static_cast<int>(name_ends.size()); i++) {
        float bri = (hover_index == i) ? 1.0f : 0.85f;
        glUniform4f(glGetUniformLocation(g_text_program, "uColor"), bri, bri, bri, 1.0f);
        glDrawArrays(GL_TRIANGLES, prev, name_ends[i] - prev);
        prev = name_ends[i];
    }

    glBindVertexArray(0);
    glDeleteBuffers(1, &tvbo); glDeleteVertexArrays(1, &tvao);

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}

// ===========================================================================
// Challenge in-game overlay (drawn on top of regular game render)
// ===========================================================================
void renderer_draw_challenge_overlay(const std::string& challenge_name,
                                     int puzzle_index, int total_puzzles,
                                     int moves_made, int max_moves,
                                     bool starts_white,
                                     int /*width*/, int /*height*/) {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Mat4 id = mat4_identity();

    // Top-center info bar
    std::vector<float> bg_verts;
    bg_verts.insert(bg_verts.end(), {-0.6f, 0.85f, 0,  0.6f, 0.85f, 0,  0.6f, 0.97f, 0,
                                       -0.6f, 0.85f, 0,  0.6f, 0.97f, 0,  -0.6f, 0.97f, 0});
    GLuint bvao, bvbo;
    glGenVertexArrays(1, &bvao); glGenBuffers(1, &bvbo);
    glBindVertexArray(bvao); glBindBuffer(GL_ARRAY_BUFFER, bvbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float)*bg_verts.size(), bg_verts.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glUseProgram(g_highlight_program);
    glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"), 1, GL_FALSE, id.m);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
    glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0, 0, 0, 0.6f);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0); glDeleteBuffers(1, &bvbo); glDeleteVertexArrays(1, &bvao);

    // Text
    std::vector<float> text_verts;
    float cw = 0.018f, ch = 0.026f;

    // Line 1: challenge name
    std::string line1 = challenge_name;
    float w1 = line1.size() * cw * 0.7f;
    add_screen_string(text_verts, -w1*0.5f, 0.945f, cw, ch, line1);
    int line1_end = static_cast<int>(text_verts.size() / 5);

    // Line 2: puzzle number / total + move counter
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Puzzle %d/%d   %s to mate in %d   Moves: %d/%d",
                  puzzle_index + 1, total_puzzles,
                  starts_white ? "White" : "Black",
                  max_moves, moves_made, max_moves);
    std::string line2 = buf;
    float w2 = line2.size() * cw * 0.7f;
    add_screen_string(text_verts, -w2*0.5f, 0.895f, cw, ch, line2);
    int line2_end = static_cast<int>(text_verts.size() / 5);

    // Line 3: hint
    std::string line3 = "ESC: reset   M: menu";
    float cw3 = 0.014f, ch3 = 0.020f;
    float w3 = line3.size() * cw3 * 0.7f;
    add_screen_string(text_verts, -w3*0.5f, -0.92f, cw3, ch3, line3);
    int line3_end = static_cast<int>(text_verts.size() / 5);

    GLuint tvao, tvbo;
    glGenVertexArrays(1, &tvao); glGenBuffers(1, &tvbo);
    glBindVertexArray(tvao); glBindBuffer(GL_ARRAY_BUFFER, tvbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float)*text_verts.size(), text_verts.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(1);

    glUseProgram(g_text_program);
    glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"), 1, GL_FALSE, id.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 1.0f, 0.9f, 0.5f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, line1_end);
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 0.9f, 0.9f, 0.9f, 1.0f);
    glDrawArrays(GL_TRIANGLES, line1_end, line2_end - line1_end);
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 0.7f, 0.7f, 0.7f, 0.8f);
    glDrawArrays(GL_TRIANGLES, line2_end, line3_end - line2_end);

    glBindVertexArray(0);
    glDeleteBuffers(1, &tvbo); glDeleteVertexArrays(1, &tvao);

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}


// ===========================================================================
// Next puzzle button
// ===========================================================================
static const float NEXT_BTN_W = 0.3f;
static const float NEXT_BTN_H = 0.10f;
static const float NEXT_BTN_X = -NEXT_BTN_W * 0.5f;
static const float NEXT_BTN_Y = -0.20f;

bool next_button_hit_test(double mx, double my, int width, int height) {
    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float ndc_y = 1.0f - 2.0f * static_cast<float>(my) / height;
    return ndc_x >= NEXT_BTN_X && ndc_x <= NEXT_BTN_X + NEXT_BTN_W &&
           ndc_y >= NEXT_BTN_Y - NEXT_BTN_H && ndc_y <= NEXT_BTN_Y;
}

void renderer_draw_next_button(int /*width*/, int /*height*/, bool hover) {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Mat4 id = mat4_identity();

    // "Solved!" label above the button
    std::vector<float> text_verts;
    float lcw = 0.045f, lch = 0.065f;
    std::string label = "Solved!";
    float lw = label.size() * lcw * 0.7f;
    add_screen_string(text_verts, -lw * 0.5f, 0.05f, lcw, lch, label);
    int label_count = static_cast<int>(text_verts.size() / 5);

    // "Next >" button text
    float bcw = 0.030f, bch = 0.045f;
    std::string btn_text = "Next >";
    float btw = btn_text.size() * bcw * 0.7f;
    add_screen_string(text_verts, -btw * 0.5f, NEXT_BTN_Y - 0.022f, bcw, bch, btn_text);
    int total_count = static_cast<int>(text_verts.size() / 5);

    // Button background
    float bg[] = {
        NEXT_BTN_X, NEXT_BTN_Y - NEXT_BTN_H, 0,
        NEXT_BTN_X + NEXT_BTN_W, NEXT_BTN_Y - NEXT_BTN_H, 0,
        NEXT_BTN_X + NEXT_BTN_W, NEXT_BTN_Y, 0,
        NEXT_BTN_X, NEXT_BTN_Y - NEXT_BTN_H, 0,
        NEXT_BTN_X + NEXT_BTN_W, NEXT_BTN_Y, 0,
        NEXT_BTN_X, NEXT_BTN_Y, 0
    };
    GLuint bvao, bvbo;
    glGenVertexArrays(1, &bvao); glGenBuffers(1, &bvbo);
    glBindVertexArray(bvao); glBindBuffer(GL_ARRAY_BUFFER, bvbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(bg), bg, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glUseProgram(g_highlight_program);
    glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"), 1, GL_FALSE, id.m);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
    glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                0.2f, 0.7f, 0.3f, hover ? 0.85f : 0.65f);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0); glDeleteBuffers(1, &bvbo); glDeleteVertexArrays(1, &bvao);

    // Draw text
    GLuint tvao, tvbo;
    glGenVertexArrays(1, &tvao); glGenBuffers(1, &tvbo);
    glBindVertexArray(tvao); glBindBuffer(GL_ARRAY_BUFFER, tvbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(text_verts.size()*sizeof(float)),
                 text_verts.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(1);

    glUseProgram(g_text_program);
    glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"), 1, GL_FALSE, id.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);

    // "Solved!" in gold
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 1.0f, 0.9f, 0.4f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, label_count);

    // Button text in white
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 1, 1, 1, 1);
    glDrawArrays(GL_TRIANGLES, label_count, total_count - label_count);

    glBindVertexArray(0); glDeleteBuffers(1, &tvbo); glDeleteVertexArrays(1, &tvao);

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}


// ===========================================================================
// Challenge summary table
// ===========================================================================
void renderer_draw_challenge_summary(const std::string& challenge_name,
                                     const std::vector<SummaryEntry>& entries,
                                     int width, int height) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glViewport(0, 0, width, height);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Mat4 id = mat4_identity();

    // Background panel
    {
        float bg[] = {-0.85f,-0.85f,0, 0.85f,-0.85f,0, 0.85f,0.85f,0,
                      -0.85f,-0.85f,0, 0.85f,0.85f,0, -0.85f,0.85f,0};
        GLuint bvao, bvbo;
        glGenVertexArrays(1, &bvao); glGenBuffers(1, &bvbo);
        glBindVertexArray(bvao); glBindBuffer(GL_ARRAY_BUFFER, bvbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(bg), bg, GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);

        glUseProgram(g_highlight_program);
        glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"), 1, GL_FALSE, id.m);
        glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
        glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0.05f, 0.07f, 0.1f, 0.9f);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0); glDeleteBuffers(1, &bvbo); glDeleteVertexArrays(1, &bvao);
    }

    std::vector<float> text_verts;
    int title_count, subtitle_end, table_start;

    // Title
    float tcw = 0.045f, tch = 0.065f;
    std::string title = "Challenge Complete!";
    float tw = title.size() * tcw * 0.7f;
    add_screen_string(text_verts, -tw*0.5f, 0.75f, tcw, tch, title);
    title_count = static_cast<int>(text_verts.size() / 5);

    // Subtitle: challenge name
    float scw = 0.022f, sch = 0.032f;
    float sw = challenge_name.size() * scw * 0.7f;
    add_screen_string(text_verts, -sw*0.5f, 0.65f, scw, sch, challenge_name);
    subtitle_end = static_cast<int>(text_verts.size() / 5);

    // Table header
    float hcw = 0.020f, hch = 0.028f;
    std::string header = "Puzzle    Your Solution";
    add_screen_string(text_verts, -0.55f, 0.52f, hcw, hch, header);
    table_start = static_cast<int>(text_verts.size() / 5);

    // Table rows
    float rcw = 0.018f, rch = 0.026f;
    float row_y = 0.45f;
    float row_h = 0.045f;
    int max_rows = 14;
    int n = std::min(static_cast<int>(entries.size()), max_rows);

    std::vector<int> row_ends;
    for (int i = 0; i < n; i++) {
        const auto& e = entries[i];
        char num[8]; std::snprintf(num, sizeof(num), "%2d.", i + 1);
        std::string row_text = num;
        row_text += "  ";
        for (size_t j = 0; j < e.moves.size(); j++) {
            if (j > 0) row_text += " ";
            row_text += e.moves[j];
        }
        if (e.moves.empty()) row_text += "(no moves)";
        add_screen_string(text_verts, -0.55f, row_y - i * row_h, rcw, rch, row_text);
        row_ends.push_back(static_cast<int>(text_verts.size() / 5));
    }

    // Footer hint
    float fcw = 0.016f, fch = 0.022f;
    std::string footer = "Click anywhere to return to menu";
    float fw = footer.size() * fcw * 0.7f;
    add_screen_string(text_verts, -fw*0.5f, -0.78f, fcw, fch, footer);
    int footer_end = static_cast<int>(text_verts.size() / 5);

    GLuint tvao, tvbo;
    glGenVertexArrays(1, &tvao); glGenBuffers(1, &tvbo);
    glBindVertexArray(tvao); glBindBuffer(GL_ARRAY_BUFFER, tvbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(text_verts.size()*sizeof(float)),
                 text_verts.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(1);

    glUseProgram(g_text_program);
    glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"), 1, GL_FALSE, id.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);

    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 1.0f, 0.9f, 0.4f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, title_count);
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 0.85f, 0.85f, 0.85f, 1.0f);
    glDrawArrays(GL_TRIANGLES, title_count, subtitle_end - title_count);
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 0.5f, 0.9f, 0.95f, 1.0f);
    glDrawArrays(GL_TRIANGLES, subtitle_end, table_start - subtitle_end);

    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 0.95f, 0.95f, 0.95f, 1.0f);
    int prev = table_start;
    for (int e : row_ends) {
        glDrawArrays(GL_TRIANGLES, prev, e - prev);
        prev = e;
    }

    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 0.6f, 0.6f, 0.6f, 0.8f);
    glDrawArrays(GL_TRIANGLES, prev, footer_end - prev);

    glBindVertexArray(0);
    glDeleteBuffers(1, &tvbo); glDeleteVertexArrays(1, &tvao);

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}
