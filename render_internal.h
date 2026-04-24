#pragma once

// Internal sharing surface between board_renderer.cpp and the
// per-screen render modules (challenge_ui, pregame, etc.) that it
// delegates to. Not part of the public API — nothing outside the
// renderer TUs should include this.

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#else
#include <epoxy/gl.h>
#endif

#include <string>
#include <vector>

// Shader programs owned by board_renderer.cpp (created in
// renderer_init). Screen modules reuse them for text / highlight /
// button draws so each screen doesn't re-compile its own program.
extern GLuint g_text_program;
extern GLuint g_highlight_program;
extern GLuint g_font_tex;

// Emit NDC-space glyph quads (pos.xyz + uv.xy per vertex, 6 verts
// per character) for a single character / a string. Coordinates are
// top-left anchored; the string routine returns the advance width.
void add_screen_char(std::vector<float>& verts, float x, float y,
                     float w, float h, char ch);
float add_screen_string(std::vector<float>& verts, float x, float y,
                        float char_w, float char_h, const std::string& str);
