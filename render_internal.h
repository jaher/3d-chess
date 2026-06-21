#pragma once

// Internal sharing surface between board_renderer.cpp and the
// per-screen render modules (challenge_ui, pregame, etc.) that it
// delegates to. Not part of the public API — nothing outside the
// renderer TUs should include this.

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
// CHESS_GLES — the OpenGL ES family (web / Android / iOS), a superset of
// __EMSCRIPTEN__. These targets render with OpenGL ES 3.0; desktop Linux /
// macOS use full GL via libepoxy. Defining it is behaviour-preserving for the
// existing Linux/web/macOS-desktop/iOS builds (each still selects the SAME GL
// header below) — it only adds the Android (__ANDROID__) branch.
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
#  ifndef CHESS_GLES
#    define CHESS_GLES 1
#  endif
#endif
#if defined(CHESS_GLES) && defined(__APPLE__) && TARGET_OS_IPHONE
// iOS / iPadOS: OpenGL ES 3.0 via Apple's OpenGLES.framework (no epoxy on iOS).
#include <OpenGLES/ES3/gl.h>
#include <OpenGLES/ES3/glext.h>
#elif defined(CHESS_GLES)
// web (emscripten) + Android NDK: OpenGL ES 3.0 system header.
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
extern GLuint g_wood_button_program;
extern GLuint g_font_tex;
// Display-only atlas baked from Cinzel-Bold; used by the main-menu
// title/subtitle. The rest of the UI samples g_font_tex (Inter).
extern GLuint g_title_font_tex;
// Walnut diffuse texture sampled by the wood-button program.
// Reused across pregame / options / challenge / endgame UIs so the
// look stays consistent without each module reloading the texture.
extern GLuint g_wood_diffuse_tex;

// Shared "stylish menu" palette so every screen lands on the same
// warm gold that the main menu introduced. The first entry is the
// resting button-label colour, the second is the hover-lit colour,
// and the third is the soft cream used for paragraph/secondary
// text. Each is a vec3 in 0-1 RGB.
struct MenuPalette {
    float label_idle[3]  = {0.808f, 0.714f, 0.467f};  // 0.95*0.85, 0.84*0.85, 0.55*0.85
    float label_hover[3] = {0.95f,  0.84f,  0.55f};
    float subtitle[3]    = {0.88f,  0.82f,  0.65f};
};
extern const MenuPalette g_menu_palette;

// Draw one chamfered-walnut button at (left, top) with size (w, h)
// in NDC. Binds g_wood_button_program + g_wood_diffuse_tex on
// TEXTURE0; caller is responsible for restoring whatever program /
// texture it had bound before. `hovered` lifts the surface a touch.
void draw_wood_button(float left, float top, float w, float h,
                      bool hovered);

// Emit NDC-space glyph quads (pos.xyz + uv.xy per vertex, 6 verts
// per character) for a single character / a string. Coordinates are
// top-left anchored; the string routine returns the advance width.
void add_screen_char(std::vector<float>& verts, float x, float y,
                     float w, float h, char ch);
float add_screen_string(std::vector<float>& verts, float x, float y,
                        float char_w, float char_h, const std::string& str);
