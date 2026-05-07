// stb_truetype-based font atlas baker for the WebAssembly build.
//
// Mirrors the layout of the desktop Cairo path (board_renderer.cpp:182):
// - Atlas is ATLAS_W x ATLAS_H pixels, single-channel R8.
// - 16 columns x 6 rows of fixed-size cells, one printable ASCII glyph
//   (32..126) per cell.
// - Each glyph is rasterized centered inside its cell.
//
// The renderer's char_uvs() helper indexes glyphs by their cell position,
// so as long as we match the cell layout exactly the desktop text shader
// sampling code works unchanged.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <GLES3/gl3.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

// Must match the constants in text_atlas.cpp; the renderer's
// char_uvs() helper assumes the same 16×6 grid so glyph UVs line
// up regardless of which baker filled the texture.
static constexpr int CELL_SIZE = 96;
static constexpr int ATLAS_COLS = 16;
static constexpr int ATLAS_FIRST_CHAR = 32;
static constexpr int FONT_PIXEL_HEIGHT = 80;  // ~83% of cell — leaves room for descenders

// Each atlas keeps its own buffered TTF data so we can hold both
// the body font (Inter, used everywhere by default) and the title
// font (Cinzel, used for the menu title + etched board labels) at
// the same time without re-reading the file every bake.
struct FontBuffer {
    unsigned char* data = nullptr;
    size_t         size = 0;
};
static FontBuffer g_body_font, g_title_font;

static bool load_font_into(FontBuffer& buf, const char* path) {
    if (buf.data) return true;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "font_atlas_stb: cannot open %s\n", path);
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    buf.size = static_cast<size_t>(std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    buf.data = static_cast<unsigned char*>(std::malloc(buf.size));
    if (!buf.data) { std::fclose(f); return false; }
    if (std::fread(buf.data, 1, buf.size, f) != buf.size) {
        std::free(buf.data);
        buf.data = nullptr;
        std::fclose(f);
        return false;
    }
    std::fclose(f);
    return true;
}

static void bake_atlas_from_buffer(const FontBuffer& buf,
                                   unsigned int* out_tex,
                                   int atlas_w, int atlas_h);

// Bake the body atlas (Inter-Bold). atlas_w / atlas_h must match
// the constants in text_atlas.cpp (768 × 288).
extern "C" void build_font_atlas_stb(unsigned int* out_tex,
                                     int atlas_w, int atlas_h) {
    if (!load_font_into(g_body_font, "/fonts/Inter-Bold.ttf")) {
        // Bind a single-pixel white texture as a fallback so the renderer
        // doesn't crash trying to sample a null texture.
        glGenTextures(1, out_tex);
        glBindTexture(GL_TEXTURE_2D, *out_tex);
        unsigned char white = 255;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE, &white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        return;
    }

    bake_atlas_from_buffer(g_body_font, out_tex, atlas_w, atlas_h);
}

// Bake the title atlas (Cinzel-Bold) — same cell layout, used by
// the menu title / subtitle and the etched board labels.
extern "C" void build_title_font_atlas_stb(unsigned int* out_tex,
                                           int atlas_w, int atlas_h) {
    if (!load_font_into(g_title_font, "/fonts/Cinzel-Bold.ttf")) {
        glGenTextures(1, out_tex);
        glBindTexture(GL_TEXTURE_2D, *out_tex);
        unsigned char white = 255;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 1, 1, 0,
                     GL_RED, GL_UNSIGNED_BYTE, &white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        return;
    }
    bake_atlas_from_buffer(g_title_font, out_tex, atlas_w, atlas_h);
}

static void bake_atlas_from_buffer(const FontBuffer& buf,
                                   unsigned int* out_tex,
                                   int atlas_w, int atlas_h) {
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, buf.data,
                        stbtt_GetFontOffsetForIndex(buf.data, 0))) {
        std::fprintf(stderr, "font_atlas_stb: stbtt_InitFont failed\n");
        return;
    }

    float scale = stbtt_ScaleForPixelHeight(&info, FONT_PIXEL_HEIGHT);
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
    int baseline = static_cast<int>(ascent * scale);
    int font_h = static_cast<int>((ascent - descent) * scale);

    unsigned char* atlas =
        static_cast<unsigned char*>(std::calloc(static_cast<size_t>(atlas_w) * atlas_h, 1));
    if (!atlas) return;

    for (int i = 0; i < 95; i++) {
        int codepoint = ATLAS_FIRST_CHAR + i;
        int col = i % ATLAS_COLS;
        int row = i / ATLAS_COLS;

        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&info, codepoint, scale, scale,
                                    &x0, &y0, &x1, &y1);
        int gw = x1 - x0;
        int gh = y1 - y0;

        int cell_x = col * CELL_SIZE;
        int cell_y = row * CELL_SIZE;
        int dst_x  = cell_x + (CELL_SIZE - gw) / 2;
        int dst_y  = cell_y + (CELL_SIZE - font_h) / 2 + baseline + y0;

        if (dst_x < 0) dst_x = 0;
        if (dst_y < 0) dst_y = 0;
        if (dst_x + gw > atlas_w) gw = atlas_w - dst_x;
        if (dst_y + gh > atlas_h) gh = atlas_h - dst_y;
        if (gw <= 0 || gh <= 0) continue;

        stbtt_MakeCodepointBitmap(&info,
            atlas + dst_y * atlas_w + dst_x,
            gw, gh, atlas_w, scale, scale, codepoint);
    }

    glGenTextures(1, out_tex);
    glBindTexture(GL_TEXTURE_2D, *out_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, atlas_w, atlas_h, 0,
                 GL_RED, GL_UNSIGNED_BYTE, atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    std::free(atlas);
}
