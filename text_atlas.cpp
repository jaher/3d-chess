#include "text_atlas.h"

#include "render_internal.h"

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
// Provided by web/font_atlas_stb.cpp; bakes the same 16x6 cell atlas
// the desktop Cairo path produces, and binds it to *out_tex with
// GL_R8.
extern "C" void build_font_atlas_stb(unsigned int* out_tex,
                                     int atlas_w, int atlas_h);
#else
#include <epoxy/gl.h>
#include <cairo.h>
#include <glib.h>
#include <pango/pangocairo.h>
#endif

#include <cstdint>

// Font atlas layout: full ASCII printable range (32-126) = 95 chars
// laid out on a 16×6 grid of 48-pixel cells.
namespace {
constexpr int CELL_SIZE        = 48;
constexpr int ATLAS_COLS       = 16;
constexpr int ATLAS_ROWS       = 6;  // ceil(95/16)
constexpr int ATLAS_W          = ATLAS_COLS * CELL_SIZE;
constexpr int ATLAS_H          = ATLAS_ROWS * CELL_SIZE;
constexpr int ATLAS_FIRST_CHAR = 32;  // space
}  // namespace

void char_uvs(char ch, float& u0, float& v0, float& u1, float& v1) {
    int idx = static_cast<int>(ch) - ATLAS_FIRST_CHAR;
    if (idx < 0 || idx >= 95) idx = 0;  // space for unknown
    int col = idx % ATLAS_COLS;
    int row = idx / ATLAS_COLS;
    u0 = static_cast<float>(col)     / ATLAS_COLS;
    v0 = static_cast<float>(row)     / ATLAS_ROWS;
    u1 = static_cast<float>(col + 1) / ATLAS_COLS;
    v1 = static_cast<float>(row + 1) / ATLAS_ROWS;
}

void build_font_atlas() {
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
    // Cairo A8 stride may differ from width; handle both cases.
    int stride = cairo_image_surface_get_stride(surface);
    if (stride == ATLAS_W) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ATLAS_W, ATLAS_H, 0, GL_RED, GL_UNSIGNED_BYTE, data);
    } else {
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

// Add a textured quad for a character in NDC (2D screen space).
void add_screen_char(std::vector<float>& verts, float x, float y,
                     float w, float h, char ch) {
    float u0, v0, u1, v1;
    char_uvs(ch, u0, v0, u1, v1);
    verts.insert(verts.end(), {x,   y-h, 0, u0, v1});
    verts.insert(verts.end(), {x+w, y-h, 0, u1, v1});
    verts.insert(verts.end(), {x+w, y,   0, u1, v0});
    verts.insert(verts.end(), {x,   y-h, 0, u0, v1});
    verts.insert(verts.end(), {x+w, y,   0, u1, v0});
    verts.insert(verts.end(), {x,   y,   0, u0, v0});
}

// Add a string of characters in NDC; returns the trailing x advance
// so callers can chain multiple segments onto the same line.
float add_screen_string(std::vector<float>& verts, float x, float y,
                        float char_w, float char_h, const std::string& str) {
    for (char ch : str) {
        add_screen_char(verts, x, y, char_w, char_h, ch);
        x += char_w * 0.7f;  // tighter spacing
    }
    return x;
}
