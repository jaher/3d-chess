#pragma once

// Bitmap font atlas used for all NDC text rendering (HUD, menu,
// challenge / pregame UI, labels, …). Built once at renderer-init
// time and stored in g_font_tex (declared in render_internal.h).
//
// add_screen_char / add_screen_string emit glyph quads against
// this atlas; they are also declared in render_internal.h because
// the per-screen modules share them.

// Build the body atlas texture (Inter Bold). Populates g_font_tex.
// Call once at renderer_init time after a GL context is available.
void build_font_atlas();

// Build the display/title atlas (Cinzel Bold). Populates
// g_title_font_tex. Used for the menu title — the rest of the UI
// stays on the body atlas for legibility at small sizes.
void build_title_font_atlas();

// UVs of a single glyph cell. Exposed so the world-space board-
// coordinate label mesh can sample the atlas directly instead of
// going through the NDC add_screen_char path.
void char_uvs(char ch, float& u0, float& v0, float& u1, float& v1);
