#include "board_renderer.h"
#include "chess_rules.h"
#include "mat.h"
#include "render_internal.h"
#include "shader.h"
#include "shatter_transition.h"
#include "splat.h"
#include "packed_splats.h"
#include "stl_model.h"
#include "text_atlas.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// GL compute tile-based gsplat rasterizer — desktop-only because
// WebGL2 (and iOS GLES) have no compute shaders. Wired behind the
// CHESS_GL_COMPUTE_SPLATS=1 env var; falls back to the existing
// per-splat-quad path otherwise. See native/gl_raster/ for the
// algorithm (Kerbl-2023 tile rasterizer ported to GLSL compute).
// iOS / iPadOS render the splat backdrop via the per-quad path (like
// web), so the GL-compute rasterizer is excluded there.
#if !defined(__EMSCRIPTEN__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
#include "gl_raster/gl_rasterizer.h"
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__EMSCRIPTEN__)
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
#elif defined(__APPLE__) && TARGET_OS_IPHONE
// iOS / iPadOS: OpenGL ES 3.0 + stb_truetype text, mirroring the web build.
// No epoxy, no Cairo/Pango, no glib — the glib monotonic timer is replaced
// with std::chrono::steady_clock (<chrono> is already included above).
#include <OpenGLES/ES3/gl.h>
#include <OpenGLES/ES3/glext.h>
extern "C" void build_font_atlas_stb(unsigned int* out_tex,
                                     int atlas_w, int atlas_h);
typedef int64_t gint64;
static inline gint64 g_get_monotonic_time() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
#else
#include <epoxy/gl.h>
#include <glib.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#endif

// ---------------------------------------------------------------------------
// GL state
// ---------------------------------------------------------------------------
static PieceGPU g_pieces[PIECE_COUNT];
// Board meshes — the chessboard ships as three pre-baked STLs
// derived from a CC-BY Sketchfab model:
//   models/board/squares_light.stl  — 32 light squares
//   models/board/squares_dark.stl   — 32 dark squares
//   models/board/frame.stl          — walnut wooden frame
// The walnut frame additionally samples two raw textures
// (diffuse + specular) via triplanar projection in the fragment
// shader; STL doesn't carry UVs but triplanar sidesteps that.
static GLuint g_board_squares_light_vao = 0,
              g_board_squares_light_vbo = 0;
static int    g_board_squares_light_count = 0;
static GLuint g_board_squares_dark_vao = 0,
              g_board_squares_dark_vbo = 0;
static int    g_board_squares_dark_count = 0;
static GLuint g_board_frame_vao = 0, g_board_frame_vbo = 0;
static int    g_board_frame_count = 0;
// Silver / chrome lining strip that runs along the frame's
// inner edge in the original Sketchfab model. Pure-white
// metallic in the source materials (Material.006 / .007).
static GLuint g_board_lining_vao = 0, g_board_lining_vbo = 0;
static int    g_board_lining_count = 0;
// 3D chess-clock model — bundled-asset Sketchfab clock (the
// CC-licensed "ChessClock" model). Each sub-object is its own
// mesh so we can shade the walnut body, the metallic needles and
// the transparent dial covers separately. The body ships with
// per-vertex UVs (custom .uvmesh format produced by
// tools/convert_clock_uvmesh.py) so its diffuse texture lands on
// the dial face correctly.
struct ClockMesh { GLuint vao = 0, vbo = 0; int count = 0; };
// Body sans hands. tools/split_clock_dials.py extracts each hand
// into its own uvmesh so the renderer can rotate them
// independently. Two hands per dial: a long minute hand mounted at
// the main dial pivot, and a short sub-dial hand mounted on its
// own offset pivot at the lower-left of the dial face. The dial
// faces (clock_dial_l/r) stay STATIC — those are the textured
// discs (numbers + knight icon).
static ClockMesh g_clock_body;
static ClockMesh g_clock_dial_l;
static ClockMesh g_clock_dial_r;
static ClockMesh g_clock_hand_long_l;
static ClockMesh g_clock_hand_long_r;
static ClockMesh g_clock_hand_short_l;
static ClockMesh g_clock_hand_short_r;
static ClockMesh g_clock_lever_l;     // silver press-down lever, slides on Y
static ClockMesh g_clock_lever_r;
static ClockMesh g_clock_glass_l;
static ClockMesh g_clock_glass_r;
// Wooden square table that the chessboard + clock sit on. Same
// uvmesh format as the clock; the conversion lives in
// tools/convert_table.py and writes to models/table/. PBR textures
// (albedo / normal / roughness / metallic / AO) ship alongside it.
// The mesh's local space puts the top surface at Y=0 so the board
// drops on it cleanly with no extra translate.
static ClockMesh g_table_mesh;
static GLuint    g_table_albedo_tex    = 0;
static GLuint    g_table_normal_tex    = 0;
static GLuint    g_table_roughness_tex = 0;
static GLuint    g_table_metallic_tex  = 0;
static GLuint    g_table_ao_tex        = 0;

// How far each lever travels when fully pressed (mesh-local units).
// The stem is ~0.128 long, so 0.04 is a clearly visible click
// without the cap disappearing into the body. Bumped from 0.04 to
// 0.15 because the smaller value reads as static on the rendered
// clock — pretty much the entire stem disappears into the body
// at full press, but that's the same way a real chess clock looks.
static constexpr float CLOCK_LEVER_PRESS = 0.15f;

// Rotation pivots in mesh-local space. Long hands pivot at the
// main dial centre at the hand's Z stack; short hands pivot at
// their own hub centroid (the hub is at the lower-Y end of the
// hand's bounding box). Rotation happens around local Z, the
// dial-face normal. Numbers come from tools/split_clock_dials.py.
static const float CLOCK_HAND_LONG_L_PIVOT[3]  = { -0.6456f, 0.6999f, 0.4036f };
static const float CLOCK_HAND_LONG_R_PIVOT[3]  = {  0.6422f, 0.6999f, 0.4036f };
static const float CLOCK_HAND_SHORT_L_PIVOT[3] = { -0.8003f, 0.5086f, 0.3930f };
static const float CLOCK_HAND_SHORT_R_PIVOT[3] = {  0.4876f, 0.5086f, 0.3929f };

// Maps cumulative thinking time on a side to a rotation angle.
// One full revolution per real-time minute — a "second-hand"
// sweep, so the motion is clearly visible on the small on-screen
// clock (a true minute-hand rate of one rev per hour is only
// 0.1°/s and looks static). Driven from monotonically-increasing
// `*_thought_ms` rather than `*_ms_left` so Fischer increments
// don't reset the needle position when a move lands.
static float dial_angle_rad(int64_t thought_ms) {
    if (thought_ms <= 0) return 0.0f;
    return static_cast<float>(thought_ms)
         * (2.0f * static_cast<float>(M_PI) / 60000.0f);
}
static GLuint    g_clock_diffuse_tex   = 0;
static GLuint    g_clock_cursor_tex    = 0;
static GLuint    g_clock_roughness_tex = 0;
static GLuint    g_clock_metalness_tex = 0;
GLuint g_wood_diffuse_tex  = 0;
static GLuint g_wood_specular_tex = 0;

// Planar-reflection FBO. The squares fragment shader samples
// this in screen space to show piece reflections on the lacquer
// surface. Color + depth are TEXTURES (not renderbuffers) — the
// depth attachment isn't sampled, but matching the format of
// g_scene_fbo (RGBA8 + DEPTH_COMPONENT24 textures) is the only
// configuration we've verified works on both desktop and web
// (WebGL2 quietly leaves a renderbuffer-depth + RGBA8-color FBO
// incomplete on some implementations even though the spec
// allows it).
static GLuint g_reflection_fbo       = 0;
static GLuint g_reflection_color_tex = 0;
static GLuint g_reflection_depth_tex = 0;
static int    g_reflection_w         = 0;
static int    g_reflection_h         = 0;
// Shared between board_renderer.cpp and the per-screen render
// modules (challenge_ui.cpp, …) via render_internal.h. Intentionally
// non-static so those TUs can link against them; no other module
// should touch these symbols.
GLuint g_text_program = 0;
GLuint g_etched_label_program = 0;
GLuint g_wood_button_program = 0;
GLuint g_highlight_program = 0;
GLuint g_font_tex = 0;
GLuint g_title_font_tex = 0;

// Shared palette for menu-style button labels and headings — same
// warm gold the main menu introduced. Owned here so the per-screen
// modules don't drift apart.
const MenuPalette g_menu_palette;

// Reusable walnut button — same chamfered look the main menu uses.
// All per-screen UIs (pregame, options, challenge, endgame) call
// this so the visual language stays consistent without each module
// re-implementing the wood-button drawing.
void draw_wood_button(float left, float top, float w, float h,
                      bool hovered) {
    static constexpr float CHAMFER = 0.012f;

    glDisable(GL_BLEND);
    glUseProgram(g_wood_button_program);
    Mat4 id = mat4_identity();
    glUniformMatrix4fv(glGetUniformLocation(g_wood_button_program, "uMVP"),
                       1, GL_FALSE, id.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_wood_diffuse_tex);
    glUniform1i(glGetUniformLocation(g_wood_button_program, "uWoodTex"), 0);
    glUniform2f(glGetUniformLocation(g_wood_button_program, "uButtonSize"),
                w, h);
    glUniform1f(glGetUniformLocation(g_wood_button_program, "uChamferSize"),
                CHAMFER);
    glUniform1f(glGetUniformLocation(g_wood_button_program, "uHoverBoost"),
                hovered ? 0.18f : 0.0f);

    float v[30] = {
        left,     top - h, 0.0f,  0.0f, 0.0f,
        left + w, top - h, 0.0f,  1.0f, 0.0f,
        left + w, top,     0.0f,  1.0f, 1.0f,
        left,     top - h, 0.0f,  0.0f, 0.0f,
        left + w, top,     0.0f,  1.0f, 1.0f,
        left,     top,     0.0f,  0.0f, 1.0f,
    };
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
    glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                          5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                          5 * sizeof(float),
                          (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glDeleteBuffers(1, &vbo); glDeleteVertexArrays(1, &vao);

    glEnable(GL_BLEND);
}

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

#ifndef __EMSCRIPTEN__
// Skybox program + panorama texture. Native-only — the web build
// keeps the original dark clear; the entire World Labs / Marble
// integration (panorama + Gaussian splats) lives behind this
// guard so chess.data and chess.wasm stay slim.
static GLuint g_skybox_program   = 0;
static GLuint g_panorama_tex     = 0;
#endif  // !__EMSCRIPTEN__

// Splat backdrop — uses the marble_viewer EWA renderer (texture-packed
// per-splat data + ordering-texture indirection + radial sort). Per
// frame the only upload is the N×4-byte ordering texture; the splat
// data sits in two RGBA32UI textures bound for the lifetime of the
// scene. Works in both desktop OpenGL and WebGL2 — the WebGL2 path
// runs the radix sort synchronously instead of on a worker thread
// (Emscripten without -pthread doesn't have std::thread).
static PackedSplats        g_packed;
static std::vector<Splat>  g_source_splats;
static GLuint              g_splat_program  = 0;
static GLuint              g_splat_quad_vbo = 0;
static GLuint              g_splat_vao      = 0;

#ifndef __EMSCRIPTEN__
// Tile-based GL compute rasterizer. Uploaded once after the SPZ
// loads — `g_gl_compute_uploaded` flips the first time we actually
// render through this path. The kernel produces visibly crisper
// output than the per-quad path in heavily-overlapped regions
// (e.g. the medieval-room interior backdrop) — see
// docs/screenshots/three_way_inside_room.png in the marble_viewer
// repo for the side-by-side.
//
// Desktop-only — WebGL2 has no compute shaders, so the web build
// stays on the per-quad path.
//
// Default: ON. Set `CHESS_GL_COMPUTE_SPLATS=0` to opt out (the per-
// quad path still ships in the binary as a fallback). The desktop
// default SPZ tier is full_res (1.9M splats) — the parallel CPU
// sort (`__gnu_parallel::sort`) now keeps even that responsive at
// 16-thread machines. Drop to 500k with `CHESS_SPLAT_TIER=500k` on
// lower-thread or older hardware.
static gl_raster::GlRasterizer g_gl_compute;
static bool                    g_gl_compute_uploaded = false;
static bool gl_compute_splats_enabled() {
    static const bool v = []() {
        const char* s = std::getenv("CHESS_GL_COMPUTE_SPLATS");
        if (!s || !*s) return true;          // default ON
        return std::atoi(s) != 0;
    }();
    return v;
}
#endif
// Robust 5–95% per-axis bbox of the splats — used when positioning
// the cloud relative to the chessboard. Filled in by load_splats().
static float g_splats_bbox_min[3] = {0, 0, 0};
static float g_splats_bbox_max[3] = {0, 0, 0};

// Per-environment scene metadata. Indexed by AppState::Environment
// (cast to int). The label is what the options-screen "Environment"
// row shows; splat_paths is a fallback chain because the desktop
// build runs from the source tree while emscripten preloads under
// the virtual `/`. splat_scale + floor_y absorb the per-scene
// tuning that lived as hardcoded constants in the splat-pass below
// — different SPZ captures have wildly different source-unit
// extents (a 5 m room vs a 90 m cathedral nave), so a one-size
// world-scale wrecks the feel.
//
// New entries: append to the end (keep enum value stable so
// settings.ini reads the right environment back). The first entry
// is the fallback when an unknown int is passed in.
struct EnvironmentDesc {
    const char* label;
    const char* splat_paths[2];
    const char* panorama_paths[2];
    float splat_scale;
    float floor_y;
};
static const EnvironmentDesc g_environments[] = {
    // 0 = MedievalRoom — original Marble tuning.
    {
        "Medieval room",
        {
#ifndef __EMSCRIPTEN__
            "world_labs/medieval_room/splat_full_res.spz",
            "world_labs/medieval_room/splat_500k.spz",
#else
            "/world_labs/medieval_room/splat_500k.spz",
            "world_labs/medieval_room/splat_500k.spz",
#endif
        },
        {
            "world_labs/medieval_room/panorama.jpg",
            "/world_labs/medieval_room/panorama.jpg",
        },
        25.0f,
        -8.878f,
    },
    // 1 = SagradaFamilia — interior canopy generated via Spaitial's
    // API (different provider from medieval_room's World Labs
    // capture, hence the separate `spaitial/` asset folder). The
    // splat_scale below is a starting guess — the cathedral source
    // bbox is much wider than the medieval room's, so the same 25×
    // world-scale would dwarf the chess table; tune visually after
    // first launch.
    {
        "Sagrada Familia",
        {
            "spaitial/sagrada_familia_interior/splat.spz",
            "/spaitial/sagrada_familia_interior/splat.spz",
        },
        {
            "spaitial/sagrada_familia_interior/panorama.jpg",
            "/spaitial/sagrada_familia_interior/panorama.jpg",
        },
        12.0f,
        -8.878f,
    },
};
constexpr int g_environment_count =
    static_cast<int>(sizeof(g_environments) / sizeof(g_environments[0]));

// Active environment — written by renderer_set_environment, read by
// the splat-pass for its scale + floor anchor. Defaults to 0
// (medieval) so behaviour before any settings load matches what the
// game used to do unconditionally.
static int g_active_environment = 0;

static const EnvironmentDesc& env_desc(int kind) {
    if (kind < 0 || kind >= g_environment_count) kind = 0;
    return g_environments[kind];
}

// Forward declarations — implementations live further down with the
// rest of the SPZ helpers / panorama loader. Hoisted here so
// renderer_set_environment (defined after them) can call into them.
const char* renderer_environment_label(int env_kind);
int renderer_environment_count();
bool renderer_set_environment(int env_kind);

// Off-screen splat-backdrop FBO. The splat draw runs here in
// isolation — its premultiplied-α blend, depth-mask-off, scissor
// state, and texture-unit bindings can't leak into the main pass
// because the main pass binds a different FBO and never sees them.
// Colour is RGBA8 single-sample; depth is DEPTH_COMPONENT24 so the
// splat shader's per-splat depth test still works for self-occlusion
// inside the cloud. Resized lazily.
static GLuint g_splat_bg_fbo       = 0;
static GLuint g_splat_bg_color_tex = 0;
static GLuint g_splat_bg_depth_tex = 0;
#ifndef __EMSCRIPTEN__
// R32F per-pixel splat-cloud surface distance (view-space), produced by
// the GL-compute rasterizer, depth-blitted into the main pass so the
// board occludes / is occluded by the room (depth-correct compositing).
// Desktop only — the GL-compute path needs compute shaders (no WebGL2).
static GLuint g_splat_surf_depth_tex = 0;
#endif
// True when g_splat_surf_depth_tex holds a valid surface-depth map for
// the current backdrop (only the GL-compute path produces it; always
// false on web, where the per-quad path keeps the flat overlay).
static bool   g_splat_surf_depth_valid = false;
static int    g_splat_bg_w         = 0;
static int    g_splat_bg_h         = 0;
// True when g_splat_bg_color_tex holds a valid splat backdrop for
// the current frame's camera. Cleared at frame start by
// renderer_begin_frame(); set after each successful splat draw. In
// multi-game mode all boards share the same camera (rot_x/rot_y/zoom
// and the room is shake-immune), so the first board pays the
// instanced-splat cost and the rest reuse the cached colour texture.
static bool   g_splat_bg_cache_valid = false;
// Splat-bg cache key for the debug light. When D-mode is on, every
// change to the light direction has to invalidate the cached splat
// texture so the shadow tracks the cursor in real time. When D-mode
// is off, light_dir_* doesn't influence the splat output, so we
// only compare against the cached "shadow enabled" flag.
static bool   g_splat_bg_cached_shadow_enabled = false;
static float  g_splat_bg_cached_light_x        = 0.0f;
static float  g_splat_bg_cached_light_y        = 0.0f;
static float  g_splat_bg_cached_light_z        = 0.0f;
// Orbit-camera pose the cached backdrop was rendered for. The room only
// moves when the camera does, so we keep the cached colour (and, on
// desktop, surface-depth) texture ACROSS frames and re-render the splats
// only when rot_x/rot_y/zoom change. Without this a static view re-sorts
// and re-draws every splat every frame — the dominant cost on the
// single-threaded web build. Sentinels force a render on the first frame.
static float  g_splat_bg_cached_rot_x = 1e30f;
static float  g_splat_bg_cached_rot_y = 1e30f;
static float  g_splat_bg_cached_zoom  = 1e30f;
// Pass-through texture-blit program used to paste g_splat_bg_color_tex
// into the main pass as a full-screen quad before the chessboard is
// drawn. Same role the panorama skybox shader fills in the no-splat
// path.
static GLuint g_splat_blit_program = 0;
#ifndef __EMSCRIPTEN__
// Depth-blit program: samples g_splat_surf_depth_tex (view-space splat
// surface distance), converts to a window-space depth via the projection
// and writes gl_FragDepth into the main pass's depth buffer, so the chess
// board can depth-test against the splat room. Desktop only.
static GLuint g_splat_depth_blit_program = 0;
#endif
// Multisample FBO that the 3D scene actually renders into. Resolved
// directly into the default framebuffer before UI overlays draw.
// GtkGLArea doesn't expose MSAA on the default FB, so we get it via
// a custom MS RBO.
//
// Web (Emscripten/WebGL2) skips this entirely — the browser canvas
// already provides MSAA via the antialias=true context attribute,
// and a custom MS FBO has format-compatibility issues with the
// default FB. The whole MS block is gated out so we don't trip
// -Wunused-function on the wasm build.
#ifndef __EMSCRIPTEN__
static GLuint g_scene_ms_fbo       = 0;
static GLuint g_scene_ms_color_rbo = 0;
static GLuint g_scene_ms_depth_rbo = 0;
static int    g_scene_ms_fbo_w     = 0;
static int    g_scene_ms_fbo_h     = 0;
constexpr int kSceneMSAASamples    = 4;
#endif
// Two-triangle NDC fullscreen quad, used by the skybox draw.
static GLuint g_fullscreen_vao   = 0;
static GLuint g_fullscreen_vbo   = 0;

// Planar-reflection FBO — color (sampled by the squares' shader)
// + depth texture (used during the mirrored pass). Both are
// textures rather than the more conventional color-tex + depth-
// renderbuffer pair, which mirrors the working g_scene_fbo
// layout and sidesteps a WebGL2 quirk where some drivers leave
// an RGBA8-color + DEPTH_COMPONENT24-renderbuffer FBO marked
// incomplete (rendering succeeds on desktop GL — the symptom on
// web is "reflections never appear" because the FBO silently
// drops the pass). Recreated lazily when (w, h) change.
static void ensure_reflection_fbo(int w, int h) {
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    if (g_reflection_fbo &&
        g_reflection_w == w && g_reflection_h == h) return;

    if (g_reflection_fbo == 0)       glGenFramebuffers(1, &g_reflection_fbo);
    if (g_reflection_color_tex == 0) glGenTextures(1, &g_reflection_color_tex);
    if (g_reflection_depth_tex == 0) glGenTextures(1, &g_reflection_depth_tex);

    glBindTexture(GL_TEXTURE_2D, g_reflection_color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, g_reflection_depth_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
                 GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindFramebuffer(GL_FRAMEBUFFER, g_reflection_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g_reflection_color_tex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, g_reflection_depth_tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr,
                "ensure_reflection_fbo: incomplete FBO 0x%x at %dx%d\n",
                st, w, h);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    g_reflection_w = w;
    g_reflection_h = h;
}

// Off-screen FBO that the splat pass renders into. Same shape as
// the reflection FBO (single-sample colour + depth textures): the
// colour texture gets sampled by g_splat_blit_program when we paste
// the backdrop into the main pass. Recreated lazily on resize.
static void ensure_splat_bg_fbo(int w, int h) {
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    if (g_splat_bg_fbo && g_splat_bg_w == w && g_splat_bg_h == h) return;

    if (g_splat_bg_fbo == 0)       glGenFramebuffers(1, &g_splat_bg_fbo);
    if (g_splat_bg_color_tex == 0) glGenTextures(1, &g_splat_bg_color_tex);
    if (g_splat_bg_depth_tex == 0) glGenTextures(1, &g_splat_bg_depth_tex);
#ifndef __EMSCRIPTEN__
    if (g_splat_surf_depth_tex == 0) glGenTextures(1, &g_splat_surf_depth_tex);
#endif

    glBindTexture(GL_TEXTURE_2D, g_splat_bg_color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

#ifndef __EMSCRIPTEN__
    // Surface-distance target for the GL-compute rasterizer's depth
    // output. R32F, NEAREST (it's sampled 1:1 by the depth-blit). Not
    // attached to g_splat_bg_fbo — the rasterizer writes it via
    // imageStore, not as a render target.
    glBindTexture(GL_TEXTURE_2D, g_splat_surf_depth_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, w, h, 0,
                 GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#endif

    glBindTexture(GL_TEXTURE_2D, g_splat_bg_depth_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
                 GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindFramebuffer(GL_FRAMEBUFFER, g_splat_bg_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g_splat_bg_color_tex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, g_splat_bg_depth_tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr,
                "ensure_splat_bg_fbo: incomplete FBO 0x%x at %dx%d\n",
                st, w, h);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    g_splat_bg_w = w;
    g_splat_bg_h = h;
    // Texture storage was just reallocated, so any cached frame is
    // gone — force the next splat draw to actually run.
    g_splat_bg_cache_valid = false;
    g_splat_surf_depth_valid = false;
}

// Call once at the start of every render frame, BEFORE any
// renderer_draw. Invalidates the splat-backdrop cache so the next
// renderer_draw will re-render it; subsequent renderer_draw calls in
// the same frame (e.g. multi-game mode) will reuse the cached
// colour texture.
extern "C" void renderer_begin_frame() {
    // Intentionally a no-op now. The splat backdrop persists across
    // frames and is re-rendered only when the camera / light / scene
    // changes (see the cache-key checks in renderer_draw). Blindly
    // invalidating here re-sorts + re-draws every splat every frame,
    // which the single-threaded web build can't afford.
}

void renderer_redraw_into_capture_fbo(GameState& gs, int width, int height,
                                       float rot_x, float rot_y, float zoom,
                                       bool human_plays_white,
                                       bool splats_enabled,
                                       float light_dir_x,
                                       float light_dir_y,
                                       float light_dir_z,
                                       bool light_positioning) {
    GLuint cap_fbo = shatter_ensure_capture_target(width, height);
    GLint prev_fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fb);

    glBindFramebuffer(GL_FRAMEBUFFER, cap_fbo);
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // The splat backdrop cache may have been invalidated by
    // renderer_begin_frame and not yet repopulated this frame in
    // the edge case where this function is called before the first
    // renderer_draw of the frame. In the normal transition flow
    // render_board has already run, so the cache is valid here and
    // renderer_draw's splat path will hit it. Either way the call
    // below does the right thing.
    renderer_draw(gs, /*sub_x=*/0, /*sub_y=*/0, width, height,
                  rot_x, rot_y, zoom,
                  human_plays_white,
                  /*endgame_menu_hover=*/false,
                  /*continue_playing_hover=*/false,
                  /*flag=*/nullptr, /*draw_flag=*/false,
                  /*withdraw_confirm_open=*/false,
                  /*withdraw_hover=*/0,
                  /*draw_clock=*/false,
                  /*clock_ms_remaining=*/0,
                  /*clock_side_is_white=*/false,
                  /*shake_x=*/0.0f,
                  /*withdraw_confirm_title=*/"Withdraw from game?",
                  /*white_thought_ms=*/0, /*black_thought_ms=*/0,
                  /*white_lever_blend=*/1.0f, /*black_lever_blend=*/0.0f,
                  /*force_panorama_only=*/!splats_enabled,
                  light_dir_x, light_dir_y, light_dir_z,
                  light_positioning);

    glBindFramebuffer(GL_FRAMEBUFFER, prev_fb);
}

// Multisample FBO used as the actual 3D-pass target. Color +
// depth are renderbuffers (we never sample them as textures —
// the resolve pass blits into the single-sample g_scene_fbo /
// default FB, which is what the outline shader / display read).
// Desktop-only — see the global-state block above for why.
#ifndef __EMSCRIPTEN__
static void ensure_scene_ms_fbo(int w, int h) {
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    if (g_scene_ms_fbo &&
        g_scene_ms_fbo_w == w && g_scene_ms_fbo_h == h) return;

    if (g_scene_ms_fbo == 0)       glGenFramebuffers (1, &g_scene_ms_fbo);
    if (g_scene_ms_color_rbo == 0) glGenRenderbuffers(1, &g_scene_ms_color_rbo);
    if (g_scene_ms_depth_rbo == 0) glGenRenderbuffers(1, &g_scene_ms_depth_rbo);

    glBindRenderbuffer(GL_RENDERBUFFER, g_scene_ms_color_rbo);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, kSceneMSAASamples,
                                     GL_RGBA8, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, g_scene_ms_depth_rbo);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, kSceneMSAASamples,
                                     GL_DEPTH_COMPONENT24, w, h);

    glBindFramebuffer(GL_FRAMEBUFFER, g_scene_ms_fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, g_scene_ms_color_rbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, g_scene_ms_depth_rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    g_scene_ms_fbo_w = w;
    g_scene_ms_fbo_h = h;
}

// Resolve g_scene_ms_fbo into `dst_fbo`. Color is averaged across
// samples (the actual MSAA work); depth picks one sample per spec
// (single-sample depth is fine for the outline shader, which only
// looks for discontinuities at pixel boundaries). Use GL_NEAREST
// — required for the depth bit and lossless for the color resolve.
//
// (dst_x, dst_y) lets callers blit into a sub-rectangle of the
// destination FB (default FB = window pixel coords). The src is
// always the full MS FBO content [0, w] × [0, h] since the MS FBO
// is sized to (w, h) for this draw — see the multi-game flow
// where each board renders into its own quadrant of the window.
static void resolve_scene_ms_to(GLuint dst_fbo,
                                int dst_x, int dst_y,
                                int w, int h,
                                bool include_depth) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_scene_ms_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_fbo);
    GLbitfield mask = GL_COLOR_BUFFER_BIT;
    if (include_depth) mask |= GL_DEPTH_BUFFER_BIT;
    glBlitFramebuffer(0, 0, w, h,
                      dst_x, dst_y, dst_x + w, dst_y + h,
                      mask, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
}
#endif  // __EMSCRIPTEN__

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

// Build a (normal, position) interleaved vertex buffer from a
// pre-positioned STL, preserving the source model's world
// coordinates. Used by the chessboard load path: the three STLs
// under models/board/ are pre-scaled to project units (8 × SQ
// across, top face at Y = 0) by the Blender splitter at
// tools/blender_split_board.py — we just need to wire them
// straight into the GL buffer.
//
// Smooth normals via angle-weighted neighbour averaging with a
// crease angle of `crease_deg` so the wooden frame's right-angle
// edges stay sharp.
static std::vector<float>
build_buffer_from_stl_world(const StlModel& m, float crease_deg) {
    const auto& tris = m.triangles();
    const size_t T = tris.size();
    std::vector<float> buf;
    buf.reserve(T * 6 * 3);

    if (crease_deg <= 0.0f) {
        // Flat-shaded fast path.
        for (const auto& tri : tris) {
            const Vertex* verts[3] = {&tri.v0, &tri.v1, &tri.v2};
            for (int i = 0; i < 3; ++i) {
                buf.push_back(tri.normal.x);
                buf.push_back(tri.normal.y);
                buf.push_back(tri.normal.z);
                buf.push_back(verts[i]->x);
                buf.push_back(verts[i]->y);
                buf.push_back(verts[i]->z);
            }
        }
        return buf;
    }

    // Quantize positions for neighbour grouping.
    auto bb = m.bounding_box();
    float ext_x = bb.max.x - bb.min.x;
    float ext_y = bb.max.y - bb.min.y;
    float ext_z = bb.max.z - bb.min.z;
    float quant = std::max({ext_x, ext_y, ext_z}) * 1e-4f;
    if (quant <= 0.0f) quant = 1e-5f;

    auto quant_key = [&](float x, float y, float z) {
        return ((static_cast<int64_t>(std::round(x / quant)) & 0x1fffff) << 42) |
               ((static_cast<int64_t>(std::round(y / quant)) & 0x1fffff) << 21) |
               ((static_cast<int64_t>(std::round(z / quant)) & 0x1fffff));
    };

    struct Vec3f { float x, y, z; };
    auto sub = [](Vec3f a, Vec3f b) -> Vec3f { return {a.x-b.x, a.y-b.y, a.z-b.z}; };
    auto cross = [](Vec3f a, Vec3f b) -> Vec3f {
        return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
    };
    auto dot = [](Vec3f a, Vec3f b) { return a.x*b.x + a.y*b.y + a.z*b.z; };
    auto len = [](Vec3f a) { return std::sqrt(a.x*a.x + a.y*a.y + a.z*a.z); };
    auto norm = [&](Vec3f a) -> Vec3f {
        float L = len(a);
        if (L < 1e-20f) return {0, 0, 0};
        return {a.x / L, a.y / L, a.z / L};
    };

    std::vector<Vec3f> face_n(T);
    std::vector<float> corner_w(T * 3);
    for (size_t t = 0; t < T; ++t) {
        const Triangle& tri = tris[t];
        Vec3f v0{tri.v0.x, tri.v0.y, tri.v0.z};
        Vec3f v1{tri.v1.x, tri.v1.y, tri.v1.z};
        Vec3f v2{tri.v2.x, tri.v2.y, tri.v2.z};
        Vec3f geo = cross(sub(v1, v0), sub(v2, v0));
        face_n[t] = (len(geo) > 1e-20f)
            ? norm(geo)
            : norm({tri.normal.x, tri.normal.y, tri.normal.z});
        auto angle_at = [&](Vec3f a, Vec3f b, Vec3f c) -> float {
            Vec3f e1 = norm(sub(b, a));
            Vec3f e2 = norm(sub(c, a));
            float d = dot(e1, e2);
            if (d > 1.0f) { d = 1.0f; }
            if (d < -1.0f) { d = -1.0f; }
            return std::acos(d);
        };
        corner_w[t * 3 + 0] = angle_at(v0, v1, v2);
        corner_w[t * 3 + 1] = angle_at(v1, v2, v0);
        corner_w[t * 3 + 2] = angle_at(v2, v0, v1);
    }

    std::unordered_map<int64_t, std::vector<std::pair<uint32_t, uint8_t>>> groups;
    groups.reserve(T * 3);
    for (size_t t = 0; t < T; ++t) {
        const Vertex* verts[3] = {&tris[t].v0, &tris[t].v1, &tris[t].v2};
        for (uint8_t i = 0; i < 3; ++i) {
            groups[quant_key(verts[i]->x, verts[i]->y, verts[i]->z)]
                .push_back({static_cast<uint32_t>(t), i});
        }
    }

    float crease_cos = std::cos(crease_deg *
                                static_cast<float>(M_PI) / 180.0f);
    for (size_t t = 0; t < T; ++t) {
        const Triangle& tri = tris[t];
        const Vertex* verts[3] = {&tri.v0, &tri.v1, &tri.v2};
        const Vec3f& fn = face_n[t];
        for (uint8_t i = 0; i < 3; ++i) {
            int64_t k = quant_key(verts[i]->x, verts[i]->y, verts[i]->z);
            const auto& grp = groups[k];
            Vec3f sum{0, 0, 0};
            for (const auto& [ot, oi] : grp) {
                const Vec3f& ofn = face_n[ot];
                if (dot(fn, ofn) < crease_cos) continue;
                float w = corner_w[ot * 3 + oi];
                sum.x += ofn.x * w;
                sum.y += ofn.y * w;
                sum.z += ofn.z * w;
            }
            Vec3f n = (len(sum) < 1e-20f) ? fn : norm(sum);
            buf.push_back(n.x);
            buf.push_back(n.y);
            buf.push_back(n.z);
            buf.push_back(verts[i]->x);
            buf.push_back(verts[i]->y);
            buf.push_back(verts[i]->z);
        }
    }
    return buf;
}

// Single-file image loader (vendored under third_party/). We
// only need the C interface from one TU.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "third_party/stb_image.h"

// Decode `path` into a GL_RGB8 GL_TEXTURE_2D and return the
// handle, or 0 on failure. Mip chain is generated; min filter
// uses the trilinear chain so triplanar projection at varying
// distances doesn't shimmer. Wrap is REPEAT so the seamless
// walnut tiles cleanly across world coordinates.
static GLuint gl_load_texture(const std::string& path) {
    int w = 0, h = 0, ch = 0;
    unsigned char* pixels =
        stbi_load(path.c_str(), &w, &h, &ch, 3 /*force RGB*/);
    if (!pixels) {
        std::fprintf(stderr,
            "[board] texture load failed: %s — %s\n",
            path.c_str(), stbi_failure_reason() ? stbi_failure_reason() : "?");
        return 0;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    std::fprintf(stderr,
        "[board] loaded texture %s — %dx%d (orig %d ch)\n",
        path.c_str(), w, h, ch);
    return tex;
}

// Set up the splat draw VAO (just one quad attribute — everything else
// comes from the splat-data textures via texelFetch in the vertex
// shader). Called once after the program is created.
static void splat_init_vao() {
    if (g_splat_vao) return;
    glGenVertexArrays(1, &g_splat_vao);
    glGenBuffers(1, &g_splat_quad_vbo);
    glBindVertexArray(g_splat_vao);
    static const float quad[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
         1.0f,  1.0f,
        -1.0f,  1.0f,
    };
    glBindBuffer(GL_ARRAY_BUFFER, g_splat_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                          2 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

// Compute the robust 5-95% per-axis bbox so we can centre the cloud
// on the chessboard regardless of stray outlier splats.
static void splat_compute_bbox(const std::vector<Splat>& splats) {
    if (splats.empty()) return;
    for (int axis = 0; axis < 3; ++axis) {
        std::vector<float> v;
        v.reserve(splats.size());
        for (const auto& sp : splats) v.push_back(sp.pos[axis]);
        std::sort(v.begin(), v.end());
        size_t lo = static_cast<size_t>(v.size() * 0.05);
        size_t hi = static_cast<size_t>(v.size() * 0.95);
        if (hi >= v.size()) hi = v.size() - 1;
        g_splats_bbox_min[axis] = v[lo];
        g_splats_bbox_max[axis] = v[hi];
    }
    std::fprintf(stderr,
        "[splat] robust bbox (5-95%%) "
        "X[%.3f, %.3f] Y[%.3f, %.3f] Z[%.3f, %.3f]\n",
        g_splats_bbox_min[0], g_splats_bbox_max[0],
        g_splats_bbox_min[1], g_splats_bbox_max[1],
        g_splats_bbox_min[2], g_splats_bbox_max[2]);
}

#ifndef __EMSCRIPTEN__
// Loader specifically for the equirectangular panorama. Same
// pipeline as gl_load_texture but with WRAP_S=REPEAT (so the
// horizontal seam wraps cleanly when the camera looks behind),
// WRAP_T=CLAMP_TO_EDGE (no wrap-around at poles), and linear
// (no mipmaps — the panorama is sampled at one mip level so
// generating mips just slows startup).
static GLuint gl_load_panorama(const std::string& path) {
    int w = 0, h = 0, ch = 0;
    unsigned char* pixels =
        stbi_load(path.c_str(), &w, &h, &ch, 3 /*force RGB*/);
    if (!pixels) {
        std::fprintf(stderr,
            "[board] panorama load failed: %s — %s\n",
            path.c_str(), stbi_failure_reason() ? stbi_failure_reason() : "?");
        return 0;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    std::fprintf(stderr,
        "[board] loaded panorama %s — %dx%d (orig %d ch)\n",
        path.c_str(), w, h, ch);
    return tex;
}
#endif  // !__EMSCRIPTEN__

// Try every path in `candidates` in order until one opens; load the
// SPZ from it, recompute the bbox, and re-upload to g_packed +
// g_source_splats. Returns true when something was actually
// (re-)loaded. The packed-splats upload deletes the old textures
// before allocating new ones, so calling this repeatedly is safe.
// Lives outside the `#ifndef __EMSCRIPTEN__` block above — both
// platforms need to (re-)load SPZs at runtime when the user picks a
// different environment.
static bool load_splat_from_candidates(const char* const* candidates,
                                       size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const char* p = candidates[i];
        if (!p || !*p) continue;
        FILE* f = std::fopen(p, "rb");
        if (!f) continue;
        std::fclose(f);
        std::vector<Splat> sp = splat_load_spz(p);
        if (sp.empty()) continue;
        splat_compute_bbox(sp);
        splat_init_vao();
        packed_splats_upload(g_packed, sp);
        g_source_splats = std::move(sp);
        return true;
    }
    return false;
}

bool renderer_set_environment(int env_kind) {
    if (env_kind < 0 || env_kind >= g_environment_count) return false;
    const EnvironmentDesc& d = g_environments[env_kind];
    const size_t n =
        sizeof(d.splat_paths) / sizeof(d.splat_paths[0]);
    if (!load_splat_from_candidates(d.splat_paths, n)) {
        std::fprintf(stderr,
            "[env] failed to load splat for '%s' — keeping previous scene\n",
            d.label);
        return false;
    }
    g_active_environment = env_kind;

#ifndef __EMSCRIPTEN__
    // GL compute path keeps its own per-load upload flag — flip it
    // so the next render() ships the freshly-uploaded splats to the
    // tile rasterizer's internal buffers.
    g_gl_compute_uploaded = false;
#endif
    // Per-frame splat backdrop cache holds the rendered colour for
    // the previous scene — invalidate so the next renderer_draw
    // re-runs the splat pass against the new cloud.
    g_splat_bg_cache_valid = false;

#ifndef __EMSCRIPTEN__
    // Reload the matching panorama too so the no-splat fallback
    // (Options → "Gaussian splats" OFF) shows the right room.
    // Desktop-only — web doesn't have the stb_image JPEG decoder
    // linked, and there's no panorama fallback there.
    if (g_panorama_tex) {
        glDeleteTextures(1, &g_panorama_tex);
        g_panorama_tex = 0;
    }
    for (const char* p : d.panorama_paths) {
        if (!p) continue;
        FILE* f = std::fopen(p, "rb");
        if (!f) continue;
        std::fclose(f);
        g_panorama_tex = gl_load_panorama(p);
        if (g_panorama_tex) break;
    }
#endif
    return true;
}

const char* renderer_environment_label(int env_kind) {
    return env_desc(env_kind).label;
}

int renderer_environment_count() {
    return g_environment_count;
}

// Load the three board STLs and the two walnut textures from
// `dir`. `dir` is "models/board" relative to the working
// directory at runtime (or "/models/board" inside the
// preload-FS for the web build, which is mounted at /). On any
// load failure the affected mesh / texture is left as 0 — the
// draw path falls back to the in-shader procedural wood for the
// frame, and a missing square mesh just won't render that side.
static void load_board_assets(const std::string& dir) {
    auto load_one = [&](const std::string& name,
                        GLuint& vao, GLuint& vbo, int& count,
                        float crease) -> bool {
        StlModel m;
        std::string path = dir + "/" + name;
        try {
            m.load(path);
        } catch (...) {
            std::fprintf(stderr,
                "[board] failed to read %s\n", path.c_str());
            return false;
        }
        if (m.triangle_count() == 0) {
            std::fprintf(stderr,
                "[board] %s has no triangles\n", path.c_str());
            return false;
        }
        std::vector<float> buf = build_buffer_from_stl_world(m, crease);
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(buf.size() * sizeof(float)),
                     buf.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              6 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
                              6 * sizeof(float),
                              (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glBindVertexArray(0);
        count = static_cast<int>(buf.size() / 6);
        std::fprintf(stderr,
            "[board] loaded %s — %zu tris → %d verts\n",
            path.c_str(), m.triangle_count(), count);
        return true;
    };

    load_one("squares_light.stl",
             g_board_squares_light_vao, g_board_squares_light_vbo,
             g_board_squares_light_count, /*crease_deg=*/30.0f);
    load_one("squares_dark.stl",
             g_board_squares_dark_vao, g_board_squares_dark_vbo,
             g_board_squares_dark_count, /*crease_deg=*/30.0f);
    load_one("frame.stl",
             g_board_frame_vao, g_board_frame_vbo,
             g_board_frame_count, /*crease_deg=*/45.0f);
    // The lining is optional — older builds of the asset pack
    // don't have it; load_one logs and returns false in that
    // case and we just skip the lining draw later on.
    load_one("frame_lining.stl",
             g_board_lining_vao, g_board_lining_vbo,
             g_board_lining_count, /*crease_deg=*/30.0f);

    g_wood_diffuse_tex  = gl_load_texture(dir + "/walnut_diffuse.jpg");
    g_wood_specular_tex = gl_load_texture(dir + "/walnut_specular.png");
}

// Load each chess-clock sub-mesh STL (body / needles / glass).
// Web ships decimated body and full-res needles + glass. The
// renderer draws each mesh with its own PBR material so the dial
// covers can be transparent and the needles can be metallic.
static void load_clock_assets(const std::string& dir) {
    auto load_one = [&](const std::string& name, ClockMesh& out,
                        float crease) -> bool {
        StlModel m;
        std::string path = dir + "/" + name;
        try {
            m.load(path);
        } catch (...) {
            std::fprintf(stderr, "[clock] failed to read %s\n", path.c_str());
            return false;
        }
        if (m.triangle_count() == 0) {
            std::fprintf(stderr, "[clock] %s has no triangles\n",
                         path.c_str());
            return false;
        }
        std::vector<float> buf = build_buffer_from_stl_world(m, crease);
        glGenVertexArrays(1, &out.vao);
        glGenBuffers(1, &out.vbo);
        glBindVertexArray(out.vao);
        glBindBuffer(GL_ARRAY_BUFFER, out.vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(buf.size() * sizeof(float)),
                     buf.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              6 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
                              6 * sizeof(float),
                              (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glBindVertexArray(0);
        out.count = static_cast<int>(buf.size() / 6);
        std::fprintf(stderr,
            "[clock] loaded %s — %zu tris → %d verts\n",
            path.c_str(), m.triangle_count(), out.count);
        return true;
    };
    const std::string suffix = ".stl";
    // Body ships in the custom UV-mesh format so the diffuse
    // texture can land on the dial face by UV instead of
    // smearing under triplanar projection. Layout per vertex
    // (8 floats, little-endian):
    //   pos.xyz  normal.xyz  uv.xy
    auto load_uvmesh = [&](const std::string& name, ClockMesh& out) -> bool {
        std::string path = dir + "/" + name;
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) {
            std::fprintf(stderr, "[clock] failed to open %s\n", path.c_str());
            return false;
        }
        char magic[4]; uint32_t vcount = 0;
        if (std::fread(magic, 1, 4, f) != 4 ||
            std::memcmp(magic, "UVME", 4) != 0 ||
            std::fread(&vcount, sizeof(vcount), 1, f) != 1) {
            std::fprintf(stderr, "[clock] bad uvmesh header %s\n",
                         path.c_str());
            std::fclose(f);
            return false;
        }
        std::vector<float> buf(vcount * 8);
        if (std::fread(buf.data(), sizeof(float), buf.size(), f) !=
            buf.size()) {
            std::fclose(f); return false;
        }
        std::fclose(f);
        glGenVertexArrays(1, &out.vao);
        glGenBuffers(1, &out.vbo);
        glBindVertexArray(out.vao);
        glBindBuffer(GL_ARRAY_BUFFER, out.vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(buf.size() * sizeof(float)),
                     buf.data(), GL_STATIC_DRAW);
        const GLsizei stride = 8 * sizeof(float);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                              (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                              (void*)(6 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glBindVertexArray(0);
        out.count = static_cast<int>(vcount);
        std::fprintf(stderr, "[clock] loaded %s — %u verts\n",
                     path.c_str(), vcount);
        return true;
    };
    const std::string uv_suffix = ".uvmesh";
    // clock_body.uvmesh on disk is post-split (hands already
    // extracted by tools/split_clock_dials.py). Re-generate via:
    //   blender --background --python tools/convert_clock_uvmesh.py
    //   python3 tools/split_clock_dials.py
    load_uvmesh("clock_body"         + uv_suffix, g_clock_body);
    load_uvmesh("clock_dial_l"       + uv_suffix, g_clock_dial_l);
    load_uvmesh("clock_dial_r"       + uv_suffix, g_clock_dial_r);
    load_uvmesh("clock_hand_long_l"  + uv_suffix, g_clock_hand_long_l);
    load_uvmesh("clock_hand_long_r"  + uv_suffix, g_clock_hand_long_r);
    load_uvmesh("clock_hand_short_l" + uv_suffix, g_clock_hand_short_l);
    load_uvmesh("clock_hand_short_r" + uv_suffix, g_clock_hand_short_r);
    load_uvmesh("clock_lever_l"      + uv_suffix, g_clock_lever_l);
    load_uvmesh("clock_lever_r"      + uv_suffix, g_clock_lever_r);
    load_one("clock_glass_r" + suffix, g_clock_glass_r, 30.0f);
    load_one("clock_glass_l" + suffix, g_clock_glass_l, 30.0f);

    g_clock_diffuse_tex   = gl_load_texture(dir + "/clock_diffuse.png");
    g_clock_cursor_tex    = gl_load_texture(dir + "/clock_cursor.png");
    g_clock_roughness_tex = gl_load_texture(dir + "/clock_roughness.png");
    g_clock_metalness_tex = gl_load_texture(dir + "/clock_metalness.png");
}

// Load the wooden table that the chessboard sits on. Same uvmesh
// format as the clock body (8-floats / vertex: pos, normal, uv);
// PBR JPEG textures alongside it. Origin is at the table's center
// of the top surface so the table can be drawn with model = identity
// and the chessboard at BOARD_Y=0 lands flush.
static void load_table_assets(const std::string& dir) {
    std::string path = dir + "/table.uvmesh";
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "[table] missing %s — skipping table\n",
                     path.c_str());
        return;
    }
    char magic[4]; uint32_t vcount = 0;
    if (std::fread(magic, 1, 4, f) != 4 ||
        std::memcmp(magic, "UVME", 4) != 0 ||
        std::fread(&vcount, sizeof(vcount), 1, f) != 1) {
        std::fprintf(stderr, "[table] bad uvmesh header %s\n", path.c_str());
        std::fclose(f);
        return;
    }
    std::vector<float> buf(static_cast<size_t>(vcount) * 8);
    if (std::fread(buf.data(), sizeof(float), buf.size(), f) != buf.size()) {
        std::fclose(f);
        return;
    }
    std::fclose(f);
    glGenVertexArrays(1, &g_table_mesh.vao);
    glGenBuffers(1, &g_table_mesh.vbo);
    glBindVertexArray(g_table_mesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_table_mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(buf.size() * sizeof(float)),
                 buf.data(), GL_STATIC_DRAW);
    const GLsizei stride = 8 * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                          (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                          (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    g_table_mesh.count = static_cast<int>(vcount);
    std::fprintf(stderr, "[table] loaded %s — %u verts\n",
                 path.c_str(), vcount);

    g_table_albedo_tex    = gl_load_texture(dir + "/table_albedo.jpg");
    g_table_normal_tex    = gl_load_texture(dir + "/table_normal.jpg");
    g_table_roughness_tex = gl_load_texture(dir + "/table_roughness.jpg");
    g_table_metallic_tex  = gl_load_texture(dir + "/table_metallic.jpg");
    g_table_ao_tex        = gl_load_texture(dir + "/table_ao.jpg");
}

static void upload_piece(PieceGPU& gpu, const StlModel& model) {
    // Always smooth per-vertex normals via build_vertex_buffer's
    // angle-weighted average. The pieces are still visibly faceted
    // at gameplay zoom even at 1M tris each — bands of flat-shaded
    // triangles show up along curved surfaces. The hash-map dedup
    // costs a few seconds at startup on the desktop hi-res meshes
    // but the visual difference is well worth it; the web build's
    // decimated meshes (~80k tris) finish in well under a second.
    // crease_angle 60° preserves intentionally sharp edges (e.g.
    // base/collar transitions) while smoothing curved regions.
    std::vector<float> buf = model.build_vertex_buffer(60.0f);
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
    g_etched_label_program = create_program(etched_vs_src, etched_fs_src);
    g_wood_button_program = create_program(wood_button_vs_src, wood_button_fs_src);
    g_splat_program       = create_program(splat_vs_src,       splat_fs_src);
    g_splat_blit_program  = create_program(splat_blit_vs_src,  splat_blit_fs_src);
#ifndef __EMSCRIPTEN__
    // Desktop-only: depth-correct compositing of the GL-compute splat
    // backdrop with the chess board (reuses the blit VS for its quad).
    g_splat_depth_blit_program =
        create_program(splat_blit_vs_src, splat_depth_blit_fs_src);
#endif
    // Marble Gaussian splat scene. Pick the heaviest tier the
    // platform can handle: full_res on desktop, 500k on web (the
    // packed-texture render path supports either; the web build
    // pays for the SPZ payload as a preload — 7.2 MB at the 500k
    // tier vs 1.2 MB at 100k — but the medieval-room interior
    // reads with much more detail at 500k, which the user is
    // actually here to see).
    {
        // Splat tier defaults (only medieval has tiers right now):
        //   * Desktop: full_res (1.9M splats). The parallel CPU sort
        //     in gl_raster/gl_rasterizer.cpp now handles this tier
        //     interactively (~50–60 fps during rotation on a 16-
        //     thread CPU). CHESS_SPLAT_TIER=500k falls back to the
        //     lighter cloud for older / lower-thread machines.
        //   * Web: 500k only. WebGL2 has no compute shaders, and
        //     the per-quad path's 1.9M splat draw stalls the JS
        //     main thread; the preload also doubles. The full_res
        //     SPZ isn't shipped with the web build (see
        //     web/Makefile's SPLAT_SPZ_PRELOAD).
        // 100k was tried and dropped — the medieval-room interior
        // reads as gappy at that tier.
        //
        // The active environment is set by app_settings_load via
        // renderer_set_environment before renderer_init returns —
        // for the very first run (no settings file yet) we fall
        // back to the medieval tier-aware path so existing users
        // see no change.
        const char* tier = std::getenv("CHESS_SPLAT_TIER");
        const char* full_paths[] = {
#ifndef __EMSCRIPTEN__
            "world_labs/medieval_room/splat_full_res.spz",
            "world_labs/medieval_room/splat_500k.spz",
#else
            "/world_labs/medieval_room/splat_500k.spz",
            "world_labs/medieval_room/splat_500k.spz",
#endif
        };
        const char* tier_500k[] = {
#ifndef __EMSCRIPTEN__
            "world_labs/medieval_room/splat_500k.spz",
            "world_labs/medieval_room/splat_full_res.spz",
#else
            "/world_labs/medieval_room/splat_500k.spz",
            "world_labs/medieval_room/splat_500k.spz",
#endif
        };
        bool want_500k = false;
#ifdef __EMSCRIPTEN__
        want_500k = true;
#endif
#ifndef __EMSCRIPTEN__
        if (tier && *tier) {
            if (std::strcmp(tier, "500k") == 0 ||
                std::strcmp(tier, "500") == 0) {
                want_500k = true;
            } else if (std::strcmp(tier, "full") == 0 ||
                       std::strcmp(tier, "fullres") == 0 ||
                       std::strcmp(tier, "full_res") == 0) {
                want_500k = false;
            }
        }
#else
        (void)tier;  // web has only 500k; env var is desktop-only.
#endif
        const char** splat_paths = want_500k ? tier_500k : full_paths;
        size_t n_paths = want_500k
            ? sizeof(tier_500k) / sizeof(*tier_500k)
            : sizeof(full_paths) / sizeof(*full_paths);
        // renderer_init always loads the medieval default (env 0);
        // on_realize swaps in the saved environment afterward if it
        // differs.
        load_splat_from_candidates(splat_paths, n_paths);
    }
#ifndef __EMSCRIPTEN__
    g_skybox_program  = create_program(skybox_vs_src,  skybox_fs_src);
    // Marble-generated panorama room. Native-only — the equirect
    // JPEG decode goes through stb_image which only the desktop
    // build links; web uses the splat backdrop above as the only
    // room renderer.
    {
        const char* candidates[] = {
            "world_labs/medieval_room/panorama.jpg",
            "/world_labs/medieval_room/panorama.jpg",
        };
        for (const char* p : candidates) {
            FILE* f = std::fopen(p, "rb");
            if (!f) continue;
            std::fclose(f);
            g_panorama_tex = gl_load_panorama(p);
            if (g_panorama_tex) break;
        }
    }
#endif  // !__EMSCRIPTEN__
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

    // Board geometry + walnut textures. Shared across desktop
    // (loaded from "models/board" relative to cwd) and the web
    // build (the Emscripten preload-FS mounts the same path at
    // /models/board — see web/Makefile's --preload-file).
#ifdef __EMSCRIPTEN__
    load_board_assets("/models/board");
    load_clock_assets("/models/clock");
    load_table_assets("/models/table");
#else
    load_board_assets("models/board");
    load_clock_assets("models/clock");
    load_table_assets("models/table");
#endif

    build_disc_mesh(0.48f, 48, g_disc_vao, g_disc_vbo, g_disc_vertex_count);
    build_ring_mesh(0.38f, 0.48f, 48, g_ring_vao, g_ring_vbo, g_ring_vertex_count);

    glEnable(GL_DEPTH_TEST);
    // Font atlases and board labels. The title atlas (Cinzel) is
    // only sampled by the main-menu title; everything else samples
    // the body atlas (Inter).
    build_font_atlas();
    build_title_font_atlas();
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

// NAG-style suffix glyph for a move-quality class — empty for the
// quiet classes (Book / Best / Excellent / Good / None) which are too
// common to annotate. Matches GameState::move_class (filled in
// app_eval_ready).
static const char* move_class_nag(MoveClass c) {
    switch (c) {
        case MoveClass::Brilliant:  return "!!";
        case MoveClass::Great:      return "!";
        case MoveClass::Inaccuracy: return "?!";
        case MoveClass::Miss:       return "?!";
        case MoveClass::MissedWin:  return "?!";
        case MoveClass::Mistake:    return "?";
        case MoveClass::Blunder:    return "??";
        default:                    return "";
    }
}

// Text tint for a move-quality class. The quiet classes fall through
// to the caller-supplied neutral grey, so good moves don't turn the
// list into a rainbow — only the notable ones get colour.
static void move_class_color(MoveClass c,
                             float nr, float ng, float nb,
                             float& r, float& g, float& b) {
    switch (c) {
        case MoveClass::Brilliant:  r = 0.10f; g = 0.85f; b = 0.78f; return; // teal
        case MoveClass::Great:      r = 0.32f; g = 0.66f; b = 1.00f; return; // blue
        case MoveClass::Inaccuracy: r = 0.96f; g = 0.83f; b = 0.28f; return; // yellow
        case MoveClass::Miss:       r = 0.98f; g = 0.70f; b = 0.20f; return; // amber
        case MoveClass::MissedWin:  r = 0.98f; g = 0.62f; b = 0.16f; return; // orange
        case MoveClass::Mistake:    r = 0.98f; g = 0.52f; b = 0.14f; return; // orange
        case MoveClass::Blunder:    r = 0.95f; g = 0.28f; b = 0.22f; return; // red
        default:                    r = nr;   g = ng;   b = nb;   return; // neutral
    }
}

// Algebraic move list below the score graph. Two columns (white /
// black) per full move number. Each move's text is tinted and
// suffixed with a NAG glyph (!!, !, ?!, ?, ??) by its quality class
// (GameState::move_class); the currently-selected move during
// analysis mode overrides everything in yellow.
static void draw_move_list(const GameState& gs) {
    if (gs.snapshots.size() <= 1) return;
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float gx0 = 0.55f, gx1 = 0.95f;
    float graph_bottom = 0.55f;
    float ml_center = (gx0 + gx1) * 0.5f;

    float ch_w = 0.022f, ch_h = 0.036f;
    float line_h = 0.040f;
    float num_w = 0.055f;
    // Wider per-column slot + gap than before so a long white SAN with
    // a NAG suffix (e.g. "Rfe1!!") no longer bleeds into the black
    // column. Mirrored in move_list_hit_test — keep the two in sync.
    float col_gap = 0.026f;
    float half_row_w = 0.092f;

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

    // Group entries by colour so the whole list still draws in a
    // handful of batches rather than one call per move. Move numbers
    // and unbadged moves share the neutral-grey bucket; each notable
    // class gets its own colour; the analysis-selected move is held
    // out and drawn last in yellow.
    const float neutral_r = 0.80f, neutral_g = 0.80f, neutral_b = 0.80f;
    struct ColorBucket { float r, g, b; std::vector<float> verts; };
    std::vector<ColorBucket> buckets;
    std::vector<float> hl_verts;  // analysis-selected move, drawn last

    auto bucket_for = [&](float r, float g, float b) -> std::vector<float>& {
        for (auto& bk : buckets)
            if (bk.r == r && bk.g == g && bk.b == b) return bk.verts;
        buckets.push_back({r, g, b, {}});
        return buckets.back().verts;
    };

    // Append one classified move to the right colour bucket (or the
    // highlight buffer when it's the analysis selection).
    auto emit_move = [&](int snap, float x, float y) {
        std::string alg = uci_to_algebraic(gs.snapshots[snap - 1],
                                            gs.snapshots[snap].last_move);
        MoveClass cls = (snap < static_cast<int>(gs.move_class.size()))
                            ? gs.move_class[snap] : MoveClass::None;
        alg += move_class_nag(cls);
        if (gs.analysis_mode && gs.analysis_index == snap) {
            add_screen_string(hl_verts, x, y, ch_w, ch_h, alg);
        } else {
            float r, g, b;
            move_class_color(cls, neutral_r, neutral_g, neutral_b, r, g, b);
            add_screen_string(bucket_for(r, g, b), x, y, ch_w, ch_h, alg);
        }
    };

    float y = ml_top;
    for (int move_num = first_move; move_num < first_move + visible_lines; move_num++) {
        int white_snap = move_num * 2 + 1;
        int black_snap = move_num * 2 + 2;

        std::string num_str = std::to_string(move_num + 1) + ".";
        add_screen_string(bucket_for(neutral_r, neutral_g, neutral_b),
                          ml_x0, y, ch_w, ch_h, num_str);

        if (white_snap <= total_moves && white_snap < static_cast<int>(gs.snapshots.size()))
            emit_move(white_snap, ml_x0 + num_w, y);

        if (black_snap <= total_moves && black_snap < static_cast<int>(gs.snapshots.size()))
            emit_move(black_snap, ml_x0 + num_w + half_row_w + col_gap, y);

        y -= line_h;
    }

    // Pack every colour bucket + the highlight buffer into one VBO and
    // draw each colour as its own range.
    std::vector<float> all_verts;
    struct DrawRange { float r, g, b; int first, count; };
    std::vector<DrawRange> ranges;
    for (auto& bk : buckets) {
        if (bk.verts.empty()) continue;
        int first = static_cast<int>(all_verts.size() / 5);
        all_verts.insert(all_verts.end(), bk.verts.begin(), bk.verts.end());
        ranges.push_back({bk.r, bk.g, bk.b, first,
                          static_cast<int>(bk.verts.size() / 5)});
    }
    if (!hl_verts.empty()) {
        int first = static_cast<int>(all_verts.size() / 5);
        all_verts.insert(all_verts.end(), hl_verts.begin(), hl_verts.end());
        ranges.push_back({1.0f, 0.9f, 0.2f, first,
                          static_cast<int>(hl_verts.size() / 5)});
    }

    if (!all_verts.empty()) {
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

        GLint cloc = glGetUniformLocation(g_text_program, "uColor");
        for (auto& rg : ranges) {
            glUniform4f(cloc, rg.r, rg.g, rg.b, 0.95f);
            glDrawArrays(GL_TRIANGLES, rg.first, rg.count);
        }

        glBindVertexArray(0);
        glDeleteBuffers(1, &mvbo); glDeleteVertexArrays(1, &mvao);
    }

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}

// Display name for a move-quality class (for the "why?" panel header).
static const char* move_class_label(MoveClass c) {
    switch (c) {
        case MoveClass::Brilliant:  return "Brilliant";
        case MoveClass::Great:      return "Great move";
        case MoveClass::Best:       return "Best move";
        case MoveClass::Excellent:  return "Excellent";
        case MoveClass::Good:       return "Good move";
        case MoveClass::Inaccuracy: return "Inaccuracy";
        case MoveClass::Miss:       return "Miss";
        case MoveClass::MissedWin:  return "Missed win";
        case MoveClass::Mistake:    return "Mistake";
        case MoveClass::Blunder:    return "Blunder";
        default:                    return "";
    }
}

// Hit-test for the move list — see board_renderer.h. Layout constants
// are kept in lockstep with draw_move_list above.
int move_list_hit_test(double mx, double my, int width, int height,
                       const GameState& gs) {
    if (gs.snapshots.size() <= 1) return -1;
    float cx = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float cy = 1.0f - 2.0f * static_cast<float>(my) / height;

    const float gx0 = 0.55f, gx1 = 0.95f;
    const float graph_bottom = 0.55f;
    const float ml_center = (gx0 + gx1) * 0.5f;
    const float ch_h = 0.036f;
    const float line_h = 0.040f;
    const float num_w = 0.055f;
    const float col_gap = 0.026f;     // keep in sync with draw_move_list
    const float half_row_w = 0.092f;

    int total_moves = static_cast<int>(gs.snapshots.size()) - 1;
    int total_full_moves = (total_moves + 1) / 2;
    int max_lines = 12;
    int first_move = 0;
    if (total_full_moves > max_lines)
        first_move = total_full_moves - max_lines;
    int visible_lines = std::min(total_full_moves - first_move, max_lines);

    const float ml_top = graph_bottom - 0.015f;
    const float row_w = num_w + half_row_w * 2 + col_gap;
    const float ml_x0 = ml_center - row_w * 0.5f;
    const float x_black = ml_x0 + num_w + half_row_w + col_gap;

    if (cx < ml_x0 || cx > ml_x0 + row_w) return -1;

    auto flagged = [&](int snap) -> bool {
        return snap >= 1 && snap <= total_moves &&
               snap < static_cast<int>(gs.snapshots.size()) &&
               snap < static_cast<int>(gs.why_reason.size()) &&
               !gs.why_reason[snap].empty();
    };

    for (int r = 0; r < visible_lines; r++) {
        int move_num = first_move + r;
        float y = ml_top - r * line_h;
        if (cy > y + 0.006f || cy < y - ch_h - 0.004f) continue;
        int snap = (cx < x_black) ? (move_num * 2 + 1)   // white col (or number)
                                  : (move_num * 2 + 2);  // black col
        return flagged(snap) ? snap : -1;
    }
    return -1;
}

// "Why?" panel NDC geometry, shared by draw_why_panel and
// why_panel_hit_test so the drawn chrome and its click targets can't
// drift apart.
namespace why_ui {
    // Panel rectangle (bottom-centre).
    constexpr float px0 = -0.52f, px1 = 0.52f, py0 = -0.97f, py1 = -0.74f;
    // Close button ("x") hit area: the panel's top-right corner.
    constexpr float close_x0 = 0.466f, close_x1 = 0.518f;
    constexpr float close_y0 = -0.810f, close_y1 = -0.758f;
}

// "Why?" explanation panel — a bottom-centre card shown when a flagged
// move's panel is open (gs.why_ply >= 0): the move + quality label, the
// engine's preferred move, a one-line reason, and an "x" close button.
// Closed by "x" / Escape / clicking off; reopen by clicking the move in
// the list again.
static void draw_why_panel(const GameState& gs, int width, int height) {
    const int P = gs.why_ply;
    const bool show_panel =
        P >= 1 && P < static_cast<int>(gs.snapshots.size()) &&
        P < static_cast<int>(gs.move_class.size());
    if (!show_panel) return;

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    Mat4 id = mat4_identity();

    glUseProgram(g_highlight_program);
    glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"), 1, GL_FALSE, id.m);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
    glUniform1i(glGetUniformLocation(g_highlight_program, "uUseGradient"), 0);
    glUniform1i(glGetUniformLocation(g_highlight_program, "uUseVertexColor"), 0);

    auto draw_rect = [&](float x0, float y0, float x1, float y1,
                         float r, float g, float b, float aa) {
        std::vector<float> v; push_quad(v, x0, y0, x1, y1);
        GLuint vao, vbo; glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
        glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(v.size()*sizeof(float)), v.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), r, g, b, aa);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(v.size()/3));
        glBindVertexArray(0); glDeleteBuffers(1, &vbo); glDeleteVertexArrays(1, &vao);
    };
    // Aspect ratio for the corner-anchored close "x" (NDC x gap shrunk
    // so its pixel margin matches the vertical one).
    const float aspect = (height > 0) ? static_cast<float>(width) / height : 1.0f;

    // Text accumulator, colour range per string.
    std::vector<float> tv;
    struct TR { int first, count; float r, g, b; };
    std::vector<TR> trs;
    auto add_text = [&](float x, float y, float cw, float ch,
                        const std::string& s, float r, float g, float b) {
        int f = static_cast<int>(tv.size() / 5);
        add_screen_string(tv, x, y, cw, ch, s);
        trs.push_back({f, static_cast<int>(tv.size()/5) - f, r, g, b});
    };

    {
        MoveClass cls = gs.move_class[P];
        std::string played = uci_to_algebraic(gs.snapshots[P - 1],
                                              gs.snapshots[P].last_move);
        played += move_class_nag(cls);
        std::string prefix = (P % 2 == 1)
            ? (std::to_string((P + 1) / 2) + ". ")
            : (std::to_string(P / 2) + "... ");
        float ar, ag, ab;
        move_class_color(cls, 0.8f, 0.8f, 0.8f, ar, ag, ab);

        const float px0 = why_ui::px0, px1 = why_ui::px1;
        const float py0 = why_ui::py0, py1 = why_ui::py1;
        draw_rect(px0-0.006f, py0-0.006f, px1+0.006f, py1+0.006f, ar, ag, ab, 0.55f); // class-coloured border
        draw_rect(px0, py0, px1, py1, 0.08f, 0.10f, 0.14f, 0.55f);                    // body
        draw_rect(px0, py1-0.012f, px1, py1, ar, ag, ab, 0.70f);                       // top accent strip
        // Close "x": drawn straight on the translucent card (no opaque
        // box). Anchor the glyph cell's top-right corner just inside the
        // panel's inner corner (below the accent strip) with equal
        // *pixel* gaps — the NDC x gap is shrunk by the aspect ratio so
        // it matches the vertical gap on screen.
        const float gap_y = 0.012f;
        const float gap_x = gap_y / aspect;
        const float xw = 0.034f, xh = 0.048f;
        float xr = px1 - gap_x;            // glyph cell right edge
        float yt = py1 - 0.012f - gap_y;   // glyph cell top edge (below accent strip)
        add_text(xr - xw, yt, xw, xh, "x", 0.92f, 0.93f, 0.96f);

        std::string line1 = prefix + played + "    " + move_class_label(cls);
        // Prefer the full principal variation ("Better line: Nf3 Nc6 Bb5");
        // fall back to just the single best move when no PV is available.
        std::string san_line;
        if (P < static_cast<int>(gs.best_pv.size()) && !gs.best_pv[P].empty())
            san_line = pv_to_san(gs.snapshots[P - 1], gs.best_pv[P], 5);
        if (san_line.empty() &&
            P < static_cast<int>(gs.best_move.size()) && !gs.best_move[P].empty())
            san_line = uci_to_algebraic(gs.snapshots[P - 1], gs.best_move[P]);
        std::string line2 = san_line.empty()
            ? std::string("Engine line unavailable")
            : ((P < static_cast<int>(gs.best_pv.size()) && !gs.best_pv[P].empty())
                   ? ("Better line:  " + san_line)
                   : ("Better was:  " + san_line));
        std::string line3 = (P < static_cast<int>(gs.why_reason.size()))
                            ? gs.why_reason[P] : std::string();
        const float tx = px0 + 0.03f;
        add_text(tx, py1 - 0.05f,  0.026f, 0.040f, line1, ar, ag, ab);
        add_text(tx, py1 - 0.108f, 0.022f, 0.034f, line2, 0.86f, 0.88f, 0.92f);
        add_text(tx, py1 - 0.160f, 0.022f, 0.034f, line3, 0.80f, 0.82f, 0.86f);
    }

    if (!tv.empty()) {
        GLuint tvao, tvbo; glGenVertexArrays(1, &tvao); glGenBuffers(1, &tvbo);
        glBindVertexArray(tvao); glBindBuffer(GL_ARRAY_BUFFER, tvbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(tv.size()*sizeof(float)), tv.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)(3*sizeof(float)));
        glEnableVertexAttribArray(1);
        glUseProgram(g_text_program);
        glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"), 1, GL_FALSE, id.m);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_font_tex);
        glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);
        GLint cloc = glGetUniformLocation(g_text_program, "uColor");
        for (const TR& t : trs) {
            glUniform4f(cloc, t.r, t.g, t.b, 1.0f);
            glDrawArrays(GL_TRIANGLES, t.first, t.count);
        }
        glBindVertexArray(0); glDeleteBuffers(1, &tvbo); glDeleteVertexArrays(1, &tvao);
    }

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}

// Hit-test for the "why?" panel's "x" close button — see
// board_renderer.h. Returns 1 if the close button was clicked (panel
// open), or 0.
int why_panel_hit_test(double mx, double my, int width, int height,
                       const GameState& gs) {
    if (gs.why_ply < 0) return 0;
    float cx = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float cy = 1.0f - 2.0f * static_cast<float>(my) / height;
    const float pad = 0.012f;   // forgiving hit area
    if (cx >= why_ui::close_x0 - pad && cx <= why_ui::close_x1 + pad &&
        cy >= why_ui::close_y0 - pad && cy <= why_ui::close_y1 + pad) return 1;
    return 0;
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
static void draw_withdraw_confirm_modal(int withdraw_hover,
                                        const char* title_text) {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Mat4 id_wc = mat4_identity();

    // 1) Full-screen dim — pulls focus to the dialog without
    // hiding the board.
    {
        glUseProgram(g_highlight_program);
        glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"),
                           1, GL_FALSE, id_wc.m);
        glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
        glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
        glUniform1i(glGetUniformLocation(g_highlight_program, "uUseGradient"), 0);
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    0.0f, 0.0f, 0.0f, 0.55f);
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

    // 2) Walnut panel — same wood treatment the menu buttons use,
    // sized as the dialog body. The chamfer reads as a chunky
    // wood plaque rather than the old flat dark-blue rectangle.
    draw_wood_button(WC_PANEL_X0,
                     WC_PANEL_Y1,
                     WC_PANEL_X1 - WC_PANEL_X0,
                     WC_PANEL_Y1 - WC_PANEL_Y0,
                     /*hovered=*/false);

    // 3) Yes / No buttons — solid colored blocks that contrast
    // against the wood (cream Yes, near-black No), echoing the
    // light/dark squares of the chess board behind them. A thin
    // top-edge highlight + bottom-edge shadow gives them the
    // same chamfer feel as the panel.
    auto draw_solid_block = [&](float x0, float y0, float x1, float y1,
                                float r, float g, float b) {
        glUseProgram(g_highlight_program);
        glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"),
                           1, GL_FALSE, id_wc.m);
        glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
        glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
        glUniform1i(glGetUniformLocation(g_highlight_program, "uUseGradient"), 0);

        // Body fill.
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    r, g, b, 1.0f);
        std::vector<float> qv; push_quad(qv, x0, y0, x1, y1);
        GLuint qvao, qvbo;
        glGenVertexArrays(1, &qvao); glGenBuffers(1, &qvbo);
        glBindVertexArray(qvao); glBindBuffer(GL_ARRAY_BUFFER, qvbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(qv.size() * sizeof(float)),
                     qv.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(qv.size() / 3));

        // Top highlight (catches the studio key light).
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    1.0f, 1.0f, 1.0f, 0.18f);
        std::vector<float> hv; push_quad(hv, x0, y1 - 0.005f, x1, y1);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(hv.size() * sizeof(float)),
                     hv.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(hv.size() / 3));

        // Bottom shadow.
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    0.0f, 0.0f, 0.0f, 0.30f);
        std::vector<float> sv; push_quad(sv, x0, y0, x1, y0 + 0.005f);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(sv.size() * sizeof(float)),
                     sv.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(sv.size() / 3));

        glBindVertexArray(0);
        glDeleteBuffers(1, &qvbo); glDeleteVertexArrays(1, &qvao);
    };

    // Yes — cream/ivory block, lifts to warmer cream on hover.
    {
        bool h = (withdraw_hover == 1);
        float r = h ? 0.96f : 0.90f;
        float g = h ? 0.91f : 0.85f;
        float b = h ? 0.78f : 0.72f;
        draw_solid_block(WC_YES_X0, WC_YES_Y0, WC_YES_X1, WC_YES_Y1, r, g, b);
    }
    // No — near-black, lifts to mid-charcoal on hover.
    {
        bool h = (withdraw_hover == 2);
        float v = h ? 0.18f : 0.06f;
        draw_solid_block(WC_NO_X0, WC_NO_Y0, WC_NO_X1, WC_NO_Y1, v, v, v);
    }

    // 4) Text — title in Cinzel, button labels in Inter. Title
    // colour is the same warm cream the menu uses for the
    // subtitle so it reads as part of the same family.
    glUseProgram(g_text_program);
    glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"),
                       1, GL_FALSE, id_wc.m);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);

    std::vector<float> title_v;
    std::string title = title_text ? title_text : "Withdraw from game?";
    float tcw = 0.034f, tch = 0.052f;
    float tw  = title.size() * tcw * 0.7f;
    add_screen_string(title_v, -tw * 0.5f, 0.08f, tcw, tch, title);

    std::vector<float> body_v;
    {
        std::string s = "Yes";
        float cw = 0.030f, ch = 0.045f;
        float sw = s.size() * cw * 0.7f;
        float cx = (WC_YES_X0 + WC_YES_X1) * 0.5f;
        float cy = (WC_YES_Y0 + WC_YES_Y1) * 0.5f;
        add_screen_string(body_v, cx - sw * 0.5f, cy + ch * 0.35f, cw, ch, s);
    }
    int yes_end = static_cast<int>(body_v.size() / 5);
    {
        std::string s = "No";
        float cw = 0.030f, ch = 0.045f;
        float sw = s.size() * cw * 0.7f;
        float cx = (WC_NO_X0 + WC_NO_X1) * 0.5f;
        float cy = (WC_NO_Y0 + WC_NO_Y1) * 0.5f;
        add_screen_string(body_v, cx - sw * 0.5f, cy + ch * 0.35f, cw, ch, s);
    }
    int no_end = static_cast<int>(body_v.size() / 5);

    auto draw_text_buf = [&](const std::vector<float>& v, GLuint tex,
                              float r, float g, float b,
                              int begin, int count) {
        if (count <= 0) return;
        GLuint tvao, tvbo;
        glGenVertexArrays(1, &tvao); glGenBuffers(1, &tvbo);
        glBindVertexArray(tvao); glBindBuffer(GL_ARRAY_BUFFER, tvbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(v.size() * sizeof(float)),
                     v.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              5 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                              5 * sizeof(float),
                              (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                    r, g, b, 1.0f);
        glDrawArrays(GL_TRIANGLES, begin, count);
        glBindVertexArray(0);
        glDeleteBuffers(1, &tvbo); glDeleteVertexArrays(1, &tvao);
    };

    // Title in Cinzel, warm cream.
    if (!title_v.empty()) {
        draw_text_buf(title_v, g_title_font_tex,
                      0.95f, 0.91f, 0.78f,
                      0, static_cast<int>(title_v.size() / 5));
    }
    // Yes label sits on the cream block — use a near-black so the
    // word reads against the light surface.
    if (!body_v.empty()) {
        draw_text_buf(body_v, g_font_tex,
                      0.10f, 0.10f, 0.10f,
                      0, yes_end);
        // No label on the dark block — use cream/white.
        draw_text_buf(body_v, g_font_tex,
                      0.95f, 0.95f, 0.95f,
                      yes_end, no_end - yes_end);
    }

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}

// "Pieces missing" modal — same visual style as draw_withdraw_confirm_modal
// (full-screen dim + outlined dark panel) but with one exit button
// instead of Yes/No, and an extra body line listing the squares
// the firmware reports as empty.
void renderer_draw_chessnut_missing_modal(const std::string& squares_msg,
                                          ChessnutBoardModalKind kind,
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

    // Title + body string depend on what's wrong.
    std::string title, body1;
    switch (kind) {
    case ChessnutBoardModalKind::Positioning:
        title = "Positioning pieces on the chessboard";
        body1 = "Wait for the motors to finish";
        break;
    case ChessnutBoardModalKind::Missing:
        title = "Place all pieces on the board";
        body1 = squares_msg.empty()
            ? std::string("No pieces detected on the board")
            : (std::string("Missing: ") + squares_msg);
        break;
    case ChessnutBoardModalKind::WrongLayout:
        title = "Place pieces in the starting position";
        body1 = "All pieces detected, but the layout is wrong";
        break;
    }

    {
        float cw = 0.028f, ch = 0.044f;
        float tw = title.size() * cw * 0.7f;
        add_screen_string(tv, -tw * 0.5f, 0.16f, cw, ch, title);
    }
    int title_count = static_cast<int>(tv.size() / 5);

    {
        float cw = 0.022f, ch = 0.034f;
        float bw = body1.size() * cw * 0.7f;
        add_screen_string(tv, -bw * 0.5f, 0.04f, cw, ch, body1);
    }
    int body1_end = static_cast<int>(tv.size() / 5);
    int body1_count = body1_end - title_count;

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
    int btn_count = btn_end - body1_end;

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
        glDrawArrays(GL_TRIANGLES, body1_end, btn_count);

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
    // While a "why?" panel is open the board is rewound into analysis
    // mode just to show the ghost move — suppress the analysis overlay
    // (dim + Continue/Back buttons) so only the panel is in view.
    const bool visible = ((gs.game_over && !gs.game_result.empty()) ||
                          gs.analysis_mode) && gs.why_ply < 0;
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

void renderer_draw_puzzle_solved_popup() {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Mat4 id = mat4_identity();
    glUseProgram(g_highlight_program);
    glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"),
                       1, GL_FALSE, id.m);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
    glUniform1i(glGetUniformLocation(g_highlight_program, "uUseGradient"), 0);

    auto draw_quad_rgba = [&](float x0, float y0, float x1, float y1,
                              float r, float g, float b, float a) {
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    r, g, b, a);
        std::vector<float> v;
        push_quad(v, x0, y0, x1, y1);
        GLuint vao = 0, vbo = 0;
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(v.size() * sizeof(float)),
                     v.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(v.size() / 3));
        glBindVertexArray(0);
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
    };

    // Dim the background a touch so the popup pops without
    // completely hiding the final position.
    draw_quad_rgba(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.45f);

    // Centered panel — a slightly-inset border + a darker inner
    // fill, with a green accent reading "Solved" semantically.
    constexpr float PX0 = -0.42f, PY0 = -0.16f;
    constexpr float PX1 =  0.42f, PY1 =  0.20f;
    draw_quad_rgba(PX0 - 0.008f, PY0 - 0.012f,
                   PX1 + 0.008f, PY1 + 0.012f,
                   0.30f, 0.65f, 0.40f, 0.95f);
    draw_quad_rgba(PX0, PY0, PX1, PY1, 0.10f, 0.18f, 0.13f, 0.97f);

    // Text.
    glUseProgram(g_text_program);
    glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"),
                       1, GL_FALSE, id.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);

    std::vector<float> tv;
    {
        // Big "Solved!" centered.
        float cw = 0.080f, ch = 0.120f;
        std::string title = "Solved!";
        float tw = title.size() * cw * 0.7f;
        add_screen_string(tv, -tw * 0.5f, 0.13f, cw, ch, title);
    }
    int title_count = static_cast<int>(tv.size() / 5);
    {
        // Smaller subtitle.
        float cw = 0.022f, ch = 0.034f;
        std::string sub = "Loading next puzzle…";
        float tw = sub.size() * cw * 0.7f;
        add_screen_string(tv, -tw * 0.5f, -0.08f, cw, ch, sub);
    }
    int total = static_cast<int>(tv.size() / 5);

    GLuint tvao = 0, tvbo = 0;
    glGenVertexArrays(1, &tvao);
    glGenBuffers(1, &tvbo);
    glBindVertexArray(tvao);
    glBindBuffer(GL_ARRAY_BUFFER, tvbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(tv.size() * sizeof(float)),
                 tv.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                          5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                          5 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // "Solved!" in bright green.
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                0.55f, 0.95f, 0.65f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, title_count);
    // Subtitle in soft white.
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                0.85f, 0.88f, 0.85f, 0.9f);
    glDrawArrays(GL_TRIANGLES, title_count, total - title_count);

    glBindVertexArray(0);
    glDeleteBuffers(1, &tvbo);
    glDeleteVertexArrays(1, &tvao);

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

void renderer_draw_active_frame(int sub_w, int sub_h) {
    // 3 px thick border, drawn as four quads. Slightly inset so
    // the lines don't bleed outside the sub-viewport when the
    // window dims aren't evenly divisible.
    constexpr float THICKNESS_PX = 3.0f;
    if (sub_w <= 0 || sub_h <= 0) return;
    float tx = THICKNESS_PX / static_cast<float>(sub_w) * 2.0f;
    float ty = THICKNESS_PX / static_cast<float>(sub_h) * 2.0f;
    const float L = -1.0f, R = 1.0f, B = -1.0f, T = 1.0f;

    std::vector<float> v;
    v.reserve(4 * 6 * 3);
    auto rect = [&](float x0, float y0, float x1, float y1) {
        v.insert(v.end(),
            {x0,y0,0,  x1,y0,0,  x1,y1,0,
             x0,y0,0,  x1,y1,0,  x0,y1,0});
    };
    rect(L,        T - ty, R,        T);          // top
    rect(L,        B,      R,        B + ty);     // bottom
    rect(L,        B + ty, L + tx,   T - ty);     // left
    rect(R - tx,   B + ty, R,        T - ty);     // right

    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(v.size() * sizeof(float)),
                 v.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float),
                          (void*)0);
    glEnableVertexAttribArray(0);

    Mat4 id = mat4_identity();
    glUseProgram(g_highlight_program);
    glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"),
                       1, GL_FALSE, id.m);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
    glUniform1i(glGetUniformLocation(g_highlight_program, "uUseGradient"), 0);
    glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                1.0f, 1.0f, 1.0f, 0.95f);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(v.size() / 3));
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);

    glBindVertexArray(0);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
}

void viewport_for_game(int game_idx, int N, int win_w, int win_h,
                       int& sub_x, int& sub_y,
                       int& sub_w, int& sub_h) {
    if (N <= 1) {
        sub_x = 0; sub_y = 0; sub_w = win_w; sub_h = win_h;
        return;
    }
    int half_w = win_w / 2;
    int half_h = win_h / 2;
    if (N == 2) {
        // Stacked left column: game 0 = top-left, game 1 = bottom-
        // left. Two wide vertical bands felt ugly at typical
        // window aspect ratios — keeping the per-board aspect
        // closer to square (i.e. quadrant-sized) reads better and
        // matches the per-board footprint at N=3/4.
        if (game_idx == 0) {
            sub_x = 0; sub_y = half_h;
            sub_w = half_w; sub_h = win_h - half_h;
        } else {
            sub_x = 0; sub_y = 0;
            sub_w = half_w; sub_h = half_h;
        }
        return;
    }
    // N == 3 or 4: 2x2 grid. Indexing top-left, top-right, bottom-left,
    // bottom-right. GL y goes bottom-up, so the top row sits at sub_y =
    // win_h/2 and the bottom row at sub_y = 0.
    int idx = game_idx;
    bool right  = (idx == 1) || (idx == 3);
    bool bottom = (idx == 2) || (idx == 3);
    sub_x = right  ? half_w : 0;
    sub_y = bottom ? 0      : half_h;
    sub_w = right  ? (win_w - half_w) : half_w;
    sub_h = bottom ? (win_h - half_h) : half_h;
}

void renderer_draw(GameState& gs,
                   int sub_x, int sub_y,
                   int width, int height,
                   float rot_x, float rot_y, float zoom,
                   bool human_plays_white,
                   bool endgame_menu_hover,
                   bool continue_playing_hover,
                   const ClothFlag* flag, bool draw_flag,
                   bool withdraw_confirm_open, int withdraw_hover,
                   bool draw_clock,
                   int64_t clock_ms_remaining,
                   bool clock_side_is_white,
                   float shake_x,
                   const char* withdraw_confirm_title,
                   int64_t white_thought_ms,
                   int64_t black_thought_ms,
                   float white_lever_blend,
                   float black_lever_blend,
                   bool force_panorama_only,
                   float light_dir_x,
                   float light_dir_y,
                   float light_dir_z,
                   bool light_positioning) {
#ifdef __EMSCRIPTEN__
    // Web build has no splat pipeline, so the toggle is unused.
    (void)force_panorama_only;
#endif
    GLint default_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &default_fbo);

    // The 3D pass renders into a multisampled FBO so we get free
    // MSAA on every silhouette and curved surface. After the 3D
    // draws we resolve straight into the default FB. NDC UI overlays
    // always draw to the default FB after the resolve so they
    // aren't affected.
    //
    // Web (Emscripten/WebGL 2) skips the custom MSAA path: the
    // browser canvas already provides MSAA for free (antialias=true
    // is the default emscripten attribute), and a custom MS FBO has
    // format-compatibility constraints with the WebGL canvas's
    // default FB that aren't always met (RGBA8/DEPTH_COMPONENT24 vs
    // the canvas's RGBA8/DEPTH24_STENCIL8). Web renders directly
    // into the default FB.
#ifdef __EMSCRIPTEN__
    const GLuint main_pass_fbo = static_cast<GLuint>(default_fbo);
#else
    ensure_scene_ms_fbo(width, height);
    const GLuint main_pass_fbo = g_scene_ms_fbo;
#endif

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
    Mat4 proj = mat4_perspective(45.0f * deg2rad, aspect, 0.1f, 250.0f);
    Mat4 vp = mat4_multiply(proj, view);

    float cd = zoom;
    float cy = BOARD_Y + cd * std::sin(-rot_x * deg2rad);
    float cxz = cd * std::cos(-rot_x * deg2rad);
    float cx = cxz * std::sin(-rot_y * deg2rad);
    float cz = cxz * std::cos(-rot_y * deg2rad);
    float view_pos[3] = {cx, cy, cz};

    // Light space — direction comes from AppState via the
    // renderer_draw signature so the D-key debug mode can aim the
    // light interactively. Defaults match the legacy values.
    float lx = light_dir_x, ly = light_dir_y, lz = light_dir_z;
    float ll = std::sqrt(lx*lx + ly*ly + lz*lz);
    if (ll < 1e-6f) { lx = 0.4f; ly = 1.0f; lz = 0.6f;
                      ll = std::sqrt(lx*lx + ly*ly + lz*lz); }
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

    // Rotate the imported Sketchfab board 90° around Y. The
    // mesh's natural square parity placed a dark square at the
    // bottom-right (h1 from white's perspective) — chess
    // convention says h1 is light. The frame is square so the
    // rotation isn't visually distinguishable, but the
    // light/dark square parity shifts to match the game's
    // expected orientation.
    Mat4 board_model = mat4_rotate_y(static_cast<float>(M_PI) / 2.0f);
    glUniformMatrix4fv(smod, 1, GL_FALSE, board_model.m);
    // Shadow pass — draw all board meshes so pieces cast a
    // shadow onto whichever surface they're sitting on (squares
    // grid + walnut frame + silver lining if present).
    glBindVertexArray(g_board_squares_light_vao);
    glDrawArrays(GL_TRIANGLES, 0, g_board_squares_light_count);
    glBindVertexArray(g_board_squares_dark_vao);
    glDrawArrays(GL_TRIANGLES, 0, g_board_squares_dark_count);
    glBindVertexArray(g_board_frame_vao);
    glDrawArrays(GL_TRIANGLES, 0, g_board_frame_count);
    if (g_board_lining_count > 0) {
        glBindVertexArray(g_board_lining_vao);
        glDrawArrays(GL_TRIANGLES, 0, g_board_lining_count);
    }
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

    // (The splat-floor shadow no longer uses the depth-map approach,
    // so the table doesn't need to be added to the shadow caster
    // set. The chessboard / pieces shadow each other via the
    // depth map; the splat floor receives the table's shadow via
    // a polygon-projection test in the splat vertex shader.)

    glDisable(GL_POLYGON_OFFSET_FILL);

    // ----- Planar-reflection pass -----
    // Render the pieces (and only the pieces) with the camera
    // mirrored about Y = 0 (the play surface) into a screen-
    // resolution FBO. The squares' fragment shader samples this
    // in screen space to show piece reflections like glass /
    // piano lacquer. Using full screen res (rather than the
    // earlier half-res) keeps the reflection sharp — visible
    // pixelation on the squares disappears once the texel
    // density matches the surface.
    //
    // Critical: glClearColor is global GL state. We save the
    // existing clear color before changing it for the reflection
    // FBO and restore it before returning, otherwise the main
    // FB's clear at the top of the main pass would inherit the
    // reflection-pass background and the menu / multi-game
    // backdrop would change colour mid-frame.
    bool reflection_ready = false;
    {
        GLfloat saved_clear[4];
        glGetFloatv(GL_COLOR_CLEAR_VALUE, saved_clear);

        const int rw = std::max(1, width);
        const int rh = std::max(1, height);
        ensure_reflection_fbo(rw, rh);
        glBindFramebuffer(GL_FRAMEBUFFER, g_reflection_fbo);
        glViewport(0, 0, rw, rh);
        // Match the same dark blue-grey the original menu uses
        // (renderer_init's initial clear) so empty regions of
        // the reflection FB don't read as a hard black hole.
        glClearColor(0.12f, 0.12f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);

        // V_reflect = V_main * S(1, -1, 1).
        Mat4 view_mirror = mat4_multiply(view, mat4_scale(1.0f, -1.0f, 1.0f));

        glUseProgram(g_program);
        glUniformMatrix4fv(glGetUniformLocation(g_program, "uView"),
                           1, GL_FALSE, view_mirror.m);
        glUniformMatrix4fv(glGetUniformLocation(g_program, "uProjection"),
                           1, GL_FALSE, proj.m);
        glUniformMatrix4fv(glGetUniformLocation(g_program, "uLightSpaceMatrix"),
                           1, GL_FALSE, light_space.m);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_shadow_tex);
        glUniform1i(glGetUniformLocation(g_program, "uShadowMap"), 0);
        // Mirrored view-pos so the BRDF's V vector lines up with
        // the mirrored geometry.
        float view_pos_mirror[3] = {view_pos[0], -view_pos[1], view_pos[2]};
        glUniform3fv(glGetUniformLocation(g_program, "uViewPos"),
                     1, view_pos_mirror);
        // Same studio rig as the main pass — kept literal here so
        // the reflection pass doesn't depend on a variable that
        // the main pass declares further down.
        const float lpos_r[12] = {0.4f,1,0.6f, -0.5f,0.8f,-0.4f,
                                  0,0.5f,-1, 0,0.5f,1};
        const float lcol_r[12] = {2.5f,2.3f,2, 1,1.1f,1.3f,
                                  0.8f,0.7f,0.6f, 0.8f,0.7f,0.6f};
        glUniform3fv(glGetUniformLocation(g_program, "uLightPositions"),
                     4, lpos_r);
        glUniform3fv(glGetUniformLocation(g_program, "uLightColors"),
                     4, lcol_r);
        glUniform1i(glGetUniformLocation(g_program, "uWoodTextureMode"), 0);
        glUniform1i(glGetUniformLocation(g_program, "uPlanarReflectionMode"), 0);
        // CRITICAL — WebGL2 flags a feedback loop whenever a sampler
        // statically references a texture that is also a current FB
        // attachment, regardless of whether the dynamic path actually
        // samples it. We're rendering to g_reflection_fbo and the
        // PBR shader's uReflectionTex sampler is otherwise still
        // pointing to TEXTURE4 (where g_reflection_color_tex is
        // bound from last frame's main pass). The driver then
        // silently drops every glDrawArrays in this pass and the
        // reflection FBO never gets populated. Redirect the sampler
        // to TEXTURE0 (shadow map — completely unrelated to this
        // FBO) for the duration of the pass; the squares' main-pass
        // draw will set it back to TEXTURE4 before sampling.
        glUniform1i(glGetUniformLocation(g_program, "uReflectionTex"), 0);

        for (const auto& bp : gs.pieces) {
            if (!bp.alive) continue;
            float wx, wz; square_center(bp.col, bp.row, wx, wz);
            float s = BASE_PIECE_SCALE * piece_scale[bp.type];
            // Use static positions even mid-AI-animation — the
            // hovering-arc lift on the moving piece is small
            // relative to the reflection's blur and the savings
            // (no per-frame state plumbing) are worth it.
            Mat4 pm = piece_model_matrix(wx, wz, s, bp.is_white, rot_z_to_y);
            float pnm[9]; mat4_normal_matrix(pm, pnm);
            glUniformMatrix4fv(glGetUniformLocation(g_program, "uModel"),
                               1, GL_FALSE, pm.m);
            glUniformMatrix3fv(glGetUniformLocation(g_program, "uNormalMat"),
                               1, GL_FALSE, pnm);
            if (bp.is_white) set_material(g_program, 0.97f, 0.95f, 0.90f, 0, 0.28f, 1, 0);
            else             set_material(g_program, 0.02f, 0.02f, 0.02f, 0, 0.35f, 1, 0);
            glBindVertexArray(g_pieces[bp.type].vao);
            glDrawArrays(GL_TRIANGLES, 0, g_pieces[bp.type].num_vertices);
            glBindVertexArray(0);
        }
        reflection_ready = true;

        // Restore the saved clear color so the main pass /
        // multi-game backdrop / menu clears keep their original
        // colour.
        glClearColor(saved_clear[0], saved_clear[1],
                     saved_clear[2], saved_clear[3]);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, main_pass_fbo);

    // --- Main pass ---
    // If main_pass_fbo is an offscreen FBO sized exactly (width,
    // height) the viewport origin is (0, 0). Otherwise (web no-
    // outline path → main_pass_fbo == default_fbo) we draw inside
    // the sub-rect on the canvas directly, so origin is (sub_x,
    // sub_y).
    //
    // Critical: when the main pass writes straight to the default
    // FB, the per-iteration glClear must be scissor-protected to
    // the sub-rect. Otherwise each multi-game iteration's clear
    // wipes the previous boards' resolved content and only the
    // last iteration's drawing survives — that's the "only the
    // bottom-right board appears with N=4" bug on the web build.
    // We don't want the scissor for the offscreen-FBO path
    // (desktop, or web cartoon-outline) because the bound FB IS
    // the sub-rect at that point and applying a default-FB-coord
    // scissor box there clipped clears for non-(0,0)-origin
    // sub-rects (the symmetric desktop-resize bug we fixed earlier).
    const bool main_pass_is_default =
        (main_pass_fbo == static_cast<GLuint>(default_fbo));
    const int mp_x = main_pass_is_default ? sub_x : 0;
    const int mp_y = main_pass_is_default ? sub_y : 0;
    glViewport(mp_x, mp_y, width, height);
    if (main_pass_is_default) {
        glScissor(sub_x, sub_y, width, height);
        glEnable(GL_SCISSOR_TEST);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
    } else {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    // Marble integration. The Gaussian-splat backdrop runs on BOTH
    // desktop and web (WebGL2 + GLSL ES 3.00 covers everything the
    // splat shader needs — unpackHalf2x16, RGBA32UI textures,
    // texelFetch, instanced draws). The fallback panorama skybox is
    // still desktop-only because its JPEG decode goes through
    // stb_image, which we only link into the desktop build.
    //
    // The splat draw runs in its OWN single-sample FBO
    // (g_splat_bg_fbo) so its premultiplied-α blend, depth-mask-off
    // scissor, and texture-unit bindings can't leak into the main
    // pass. After the splat pass we re-bind the main FBO and paste
    // g_splat_bg_color_tex as a full-screen quad — same role the
    // panorama skybox shader plays in the no-splat path.
    if (!force_panorama_only && g_packed.count > 0 && g_splat_program) {
        // Camera direction (world-space) — used by Spark's "did the
        // camera move enough to re-sort?" early-out.
        float vd[3] = {-cx, -cy, -cz};
        float vl = std::sqrt(vd[0]*vd[0] + vd[1]*vd[1] + vd[2]*vd[2]);
        if (vl > 1e-6f) { vd[0] /= vl; vd[1] /= vl; vd[2] /= vl; }

        // Splat-cloud world transform. Marble's coordinate system
        // has -Y as up; we flip Y. Scale + translate so the
        // ROBUST percentile bbox (computed at load) centres on
        // the chessboard origin in X/Z and the floor sits a few
        // units below the chessboard plane.
        // Splat-cloud world transform. Marble's coordinate system has
        // -Y as up; the negative Y in mat4_scale flips it. The room
        // is centred on the chessboard (X/Z bbox midpoint → world
        // origin) and the floor is anchored to the TABLE's bottom
        // (Y_floor = -8.878, which is BOARD_Y + TABLE_TOP_Y -
        // table_height = 0 - 0.608 - 8.27) so the table reads as
        // sitting on the splat-room floor.
        //
        // Per-environment scale + floor anchor. The original
        // medieval-room tuning (25× scale, floor at y=-8.878) lived
        // here as constants; multi-environment support folds those
        // into g_environments[] so each captured scene can carry
        // its own world-space scale (a cathedral nave needs a
        // smaller multiplier than a small study). Scale tuned so
        // the table reads as a piece of furniture, not a stage in
        // a theatre — table footprint is ~14 world units, so the
        // room's projected footprint should land around 50–60
        // units for that proportion.
        const EnvironmentDesc& env = env_desc(g_active_environment);
        const float splat_scale  = env.splat_scale;
        const float floor_world_y = env.floor_y;
        const float* mn = g_splats_bbox_min;
        const float* mx = g_splats_bbox_max;
        const float splat_cx = -(mn[0] + mx[0]) * 0.5f * splat_scale;
        const float splat_cy = floor_world_y - (-mx[1] * splat_scale);
        const float splat_cz = -(mn[2] + mx[2]) * 0.5f * splat_scale;
        Mat4 splat_model = mat4_multiply(
            mat4_translate(splat_cx, splat_cy, splat_cz),
            mat4_scale(splat_scale, -splat_scale, splat_scale));

        // ----- Render splats into the dedicated bg FBO (skipped if
        // an earlier board in this frame already populated it) -----
        ensure_splat_bg_fbo(width, height);
        // The cached splat texture is light-dependent only while
        // D-mode is on. Invalidate when (shadow_enabled, light_dir)
        // differs from the cached key so the user sees the shadow
        // track the cursor in real time.
        if (g_splat_bg_cache_valid) {
            const bool same_enabled =
                g_splat_bg_cached_shadow_enabled == light_positioning;
            const bool same_dir = !light_positioning ||
                (g_splat_bg_cached_light_x == light_dir_x &&
                 g_splat_bg_cached_light_y == light_dir_y &&
                 g_splat_bg_cached_light_z == light_dir_z);
            // Re-render when the camera moves — the room view changes
            // with rot_x/rot_y/zoom (exact compare: any motion misses).
            const bool same_cam =
                g_splat_bg_cached_rot_x == rot_x &&
                g_splat_bg_cached_rot_y == rot_y &&
                g_splat_bg_cached_zoom  == zoom;
            if (!same_enabled || !same_dir || !same_cam)
                g_splat_bg_cache_valid = false;
        }
        if (!g_splat_bg_cache_valid) {
            // Record the camera this (re)render is for, so subsequent
            // static frames reuse the cached backdrop instead of
            // re-sorting + re-drawing every splat.
            g_splat_bg_cached_rot_x = rot_x;
            g_splat_bg_cached_rot_y = rot_y;
            g_splat_bg_cached_zoom  = zoom;
            // The room is shake-immune — board_shake is meant to
            // wobble the table, not the world around it. Build a
            // shake-less view here so the cached splat output is
            // identical for every board in a multi-game frame
            // (otherwise the active board's shake would also smear
            // the backdrop reused by inactive boards).
            Mat4 view_no_shake = mat4_multiply(
                mat4_translate(0, 0, -zoom),
                mat4_multiply(mat4_rotate_x(rot_x * deg2rad),
                              mat4_multiply(mat4_rotate_y(rot_y * deg2rad),
                                            mat4_translate(0, -BOARD_Y, 0))));

            // Radial sort using the camera's world position. (The
            // per-quad path uses this for its global depth-sort; the
            // GL compute path below does its own per-tile sort, so
            // the sort+upload here is wasted there — but it's cheap
            // and keeping it means CHESS_GL_COMPUTE_SPLATS can be
            // toggled mid-run without re-loading the splats.)
            float cam_pos[3] = { cx, cy, cz };
            packed_splats_sort_and_upload(g_packed, cam_pos, vd,
                                          splat_model.m, g_source_splats);

#ifndef __EMSCRIPTEN__
            // ── CHESS_GL_COMPUTE_SPLATS=1: route through the
            // tile-based GL compute rasterizer (see gl_raster/).
            // Quality upgrade: cleaner edges + less smearing in
            // heavy-overlap regions like the medieval-room
            // interior backdrop. Desktop-only.
            if (gl_compute_splats_enabled()) {
                if (!g_gl_compute_uploaded) {
                    // The chess world flips Marble's Y. The per-quad
                    // path absorbs this via splat_model = T·S(s,-s,s)
                    // (a reflection); the GL compute path's preprocess
                    // kernel uses a rotation-only Shoemake decomp and
                    // can't represent a reflection, so we pre-flip Y
                    // in the uploaded SplatGPU data and pass a
                    // positive-Y model below.
                    g_gl_compute.upload(g_source_splats, /*flip_y=*/true);
                    g_gl_compute_uploaded = true;
                }
                g_gl_compute.resize(width, height);
                g_gl_compute.set_output_texture(g_splat_bg_color_tex);
                // Also emit the per-pixel surface distance so the board
                // can depth-test against the room (depth-correct compositing,
                // depth-blitted after the colour paste below).
                g_gl_compute.set_depth_output_texture(g_splat_surf_depth_tex);
                // GL compute model: same translation + scale as the
                // per-quad path but with positive Y scale (the Y-flip
                // is baked into the splat data on upload above).
                Mat4 splat_model_compute = mat4_multiply(
                    mat4_translate(splat_cx, splat_cy, splat_cz),
                    mat4_scale(splat_scale, splat_scale, splat_scale));
                g_gl_compute.render(view_no_shake.m, proj.m,
                                    splat_model_compute.m);
                // Skip the per-quad draw — the compute kernel
                // already filled g_splat_bg_color_tex.
                g_splat_bg_cache_valid = true;
                g_splat_surf_depth_valid = true;
                g_splat_bg_cached_shadow_enabled = light_positioning;
                g_splat_bg_cached_light_x = light_dir_x;
                g_splat_bg_cached_light_y = light_dir_y;
                g_splat_bg_cached_light_z = light_dir_z;
                // Re-bind main FBO + reset state below expects to
                // jump past the per-quad block.
                goto skip_per_quad_splat_draw;
            }
#endif

            glBindFramebuffer(GL_FRAMEBUFFER, g_splat_bg_fbo);
            glViewport(0, 0, width, height);
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            glUseProgram(g_splat_program);
            glUniformMatrix4fv(glGetUniformLocation(g_splat_program, "uView"),
                               1, GL_FALSE, view_no_shake.m);
            glUniformMatrix4fv(glGetUniformLocation(g_splat_program, "uProjection"),
                               1, GL_FALSE, proj.m);
            glUniformMatrix4fv(glGetUniformLocation(g_splat_program, "uModel"),
                               1, GL_FALSE, splat_model.m);
            glUniform1f(glGetUniformLocation(g_splat_program, "uModelScale"),
                        splat_scale);
            glUniform2f(glGetUniformLocation(g_splat_program, "uRenderSize"),
                        static_cast<float>(width),
                        static_cast<float>(height));
            glUniform1f(glGetUniformLocation(g_splat_program, "uMaxStdDev"),
                        std::sqrt(8.0f));
            glUniform1f(glGetUniformLocation(g_splat_program, "uMaxPixelRadius"), 512.0f);
            glUniform1f(glGetUniformLocation(g_splat_program, "uMinAlpha"), 1.0f / 255.0f);
            glUniform1f(glGetUniformLocation(g_splat_program, "uFalloff"), 1.0f);
            glUniform1i(glGetUniformLocation(g_splat_program, "uEnable2DGS"), 0);
            glUniform1i(glGetUniformLocation(g_splat_program, "uLogDepth"), 0);
            glUniform1i(glGetUniformLocation(g_splat_program, "uTexWidth"),
                        g_packed.tex_width);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_packed.texA);
            glUniform1i(glGetUniformLocation(g_splat_program, "uSplatA"), 0);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, g_packed.texB);
            glUniform1i(glGetUniformLocation(g_splat_program, "uSplatB"), 1);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, g_packed.texOrder);
            glUniform1i(glGetUniformLocation(g_splat_program, "uOrdering"), 2);

            // Table-shadow uniforms (debug — only meaningful while
            // D-mode is on). No texture binding required: the splat
            // shader tests against an axis-aligned 14×14 rectangle
            // at the table top Y, traced from the splat along the
            // light direction. Clean polygonal shadow, no depth-map
            // artefacts. The uShadowEnabled gate keeps the test
            // dormant when D is off.
            glUniform1f(glGetUniformLocation(g_splat_program, "uShadowEnabled"),
                        light_positioning ? 1.0f : 0.0f);
            if (light_positioning) {
                constexpr float TABLE_TOP_Y = -0.608f;
                glUniform3f(
                    glGetUniformLocation(g_splat_program, "uShadowLightDir"),
                    lx, ly, lz);
                glUniform1f(
                    glGetUniformLocation(g_splat_program, "uShadowTableTopY"),
                    TABLE_TOP_Y);
                glUniform2f(
                    glGetUniformLocation(g_splat_program, "uShadowTableHalfExtent"),
                    7.0f, 7.0f);
            }

            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glBindVertexArray(g_splat_vao);
            glDrawArraysInstanced(GL_TRIANGLE_FAN, 0, 4, g_packed.count);
            glBindVertexArray(0);

            // The splat shader sampled g_packed.texA / texB / texOrder
            // as RGBA32UI / R32UI integer textures on units 0/1/2.
            // Leaving those integer textures bound on units 1/2 is
            // dangerous — if any later shader's sampler2D defaults to
            // those units (or gets glUniform1i'd to one of them and we
            // forgot to set the expected float texture), it samples
            // garbage. Unbind both proactively. Unit 0 gets re-bound
            // by the blit below.
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, 0);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, 0);
            glActiveTexture(GL_TEXTURE0);

            g_splat_bg_cache_valid = true;
            // The per-quad path produces no surface-depth map, so the
            // board falls back to drawing over the flat backdrop.
            g_splat_surf_depth_valid = false;
            g_splat_bg_cached_shadow_enabled = light_positioning;
            g_splat_bg_cached_light_x = light_dir_x;
            g_splat_bg_cached_light_y = light_dir_y;
            g_splat_bg_cached_light_z = light_dir_z;
#ifndef __EMSCRIPTEN__
        skip_per_quad_splat_draw: ;
#endif
        }

        // ----- Done with splat-bg FBO. Rebind main pass + paste -----
        glBindFramebuffer(GL_FRAMEBUFFER, main_pass_fbo);
        glViewport(mp_x, mp_y, width, height);
        // Reset every state the splat pass touched. Subsequent
        // chessboard / piece / UI passes assume the panorama-pass
        // exit state: blend OFF, depth test ON with depth writes ON,
        // scissor disabled. The blit shader below sets its own
        // depth-test-off + blend-off too, then we restore.
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_DEPTH_TEST);
        if (main_pass_is_default) {
            glScissor(sub_x, sub_y, width, height);
            glEnable(GL_SCISSOR_TEST);
        }

        // Paste the splat colour into the main pass as a fullscreen
        // quad. Pure overwrite — no alpha, no depth, no shading.
        {
            glUseProgram(g_splat_blit_program);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_splat_bg_color_tex);
            glUniform1i(glGetUniformLocation(g_splat_blit_program, "uTex"), 0);
            glBindVertexArray(g_fullscreen_vao);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glBindVertexArray(0);
        }

#ifndef __EMSCRIPTEN__
        // Depth-correct compositing: stamp the GL-compute backdrop's
        // surface distance into the main pass depth buffer (gl_FragDepth)
        // so the board below depth-tests against the room instead of
        // always drawing on top. Only the GL-compute path produces the
        // surface map; the per-quad path keeps the flat-overlay look.
        // Runs with the same viewport + scissor as the colour paste.
        if (g_splat_surf_depth_valid && g_splat_depth_blit_program &&
            g_splat_surf_depth_tex) {
            glUseProgram(g_splat_depth_blit_program);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_splat_surf_depth_tex);
            glUniform1i(glGetUniformLocation(g_splat_depth_blit_program,
                                             "uSurfDist"), 0);
            glUniform1f(glGetUniformLocation(g_splat_depth_blit_program,
                                             "uProjA"), proj.m[10]);
            glUniform1f(glGetUniformLocation(g_splat_depth_blit_program,
                                             "uProjB"), proj.m[14]);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_ALWAYS);     // overwrite depth everywhere
            glDepthMask(GL_TRUE);
            glBindVertexArray(g_fullscreen_vao);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glBindVertexArray(0);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthFunc(GL_LESS);       // restore for the board pass
        }
#endif

        // Leave TEXTURE0 unbound so it can't double as a stale binding
        // for the next pass. The chessboard pass sets uShadowMap →
        // TEXTURE0 = g_shadow_tex right after this anyway.
        glBindTexture(GL_TEXTURE_2D, 0);

        if (main_pass_is_default) glDisable(GL_SCISSOR_TEST);
        glEnable(GL_DEPTH_TEST);
    }
#ifndef __EMSCRIPTEN__
    else
    // Skybox pass — fills the scene with the Marble-generated
    // panorama before any 3D geometry, so the chessboard + clock
    // appear to sit inside the medieval room. Drawn with depth
    // writes off and at the far plane (z=1.0); subsequent geometry
    // overdraws it where present. Desktop-only because we only link
    // stb_image (the JPEG decoder for panorama.jpg) on desktop.
    if (g_panorama_tex && g_skybox_program) {
        Mat4 inv_proj = mat4_inverse(proj);
        // The view matrix's rotation-only part is R_x(rot_x)·R_y(rot_y).
        // Its inverse (orthonormal => transpose) is R_y(-rot_y)·R_x(-rot_x).
        Mat4 inv_view_rot = mat4_multiply(
            mat4_rotate_y(-rot_y * deg2rad),
            mat4_rotate_x(-rot_x * deg2rad));
        glUseProgram(g_skybox_program);
        glUniformMatrix4fv(glGetUniformLocation(g_skybox_program, "uInvProj"),
                           1, GL_FALSE, inv_proj.m);
        glUniformMatrix4fv(glGetUniformLocation(g_skybox_program, "uInvViewRot"),
                           1, GL_FALSE, inv_view_rot.m);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_panorama_tex);
        glUniform1i(glGetUniformLocation(g_skybox_program, "uPano"), 0);
        GLboolean depth_was_on = glIsEnabled(GL_DEPTH_TEST);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        if (main_pass_is_default) {
            glScissor(sub_x, sub_y, width, height);
            glEnable(GL_SCISSOR_TEST);
        }
        glBindVertexArray(g_fullscreen_vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
        if (main_pass_is_default) glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);
        if (depth_was_on) glEnable(GL_DEPTH_TEST);
    }
#endif  // !__EMSCRIPTEN__

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

    // ----- Table the chessboard sits on -----
    // The wooden square table renders before the board so the board
    // mesh / pieces / clock overdraw it where they overlap. The
    // mesh's local space has the top surface at Y=0, but in world
    // space the chessboard frame's underside is at Y=-0.608 (frame
    // STL spans Y[-0.608, -0.020] — the existing clock is already
    // translated to Y=-0.608 to "sit on the same notional table
    // surface as the board"; this is now the actual table). Drop
    // the table top to that plane so the chessboard's bottom AND
    // the clock's base both rest on it cleanly. No change needed
    // to the board / clock model matrices — they were already
    // calibrated to this Y.
    constexpr float TABLE_TOP_Y = -0.608f;
    if (g_table_mesh.count > 0 && g_table_albedo_tex) {
        Mat4 table_model = mat4_translate(0.0f, TABLE_TOP_Y, 0.0f);
        float tnm[9]; mat4_normal_matrix(table_model, tnm);
        glUniformMatrix4fv(glGetUniformLocation(g_program, "uModel"),
                           1, GL_FALSE, table_model.m);
        glUniformMatrix3fv(glGetUniformLocation(g_program, "uNormalMat"),
                           1, GL_FALSE, tnm);
        glUniform1i(glGetUniformLocation(g_program, "uWoodTextureMode"), 0);
        glUniform1i(glGetUniformLocation(g_program, "uPlanarReflectionMode"), 0);
        glUniform1i(glGetUniformLocation(g_program, "uClockTextureMode"), 1);
        glUniform1i(glGetUniformLocation(g_program, "uClockPbrMapsMode"), 1);
        glUniform1f(glGetUniformLocation(g_program, "uMaterialOpacity"), 1.0f);
        // (1, 1, 1) so the texture comes through with no tint.
        set_material(g_program, 1.0f, 1.0f, 1.0f, 0.0f, 0.65f, 1.0f, 0);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, g_table_albedo_tex);
        glUniform1i(glGetUniformLocation(g_program, "uClockDiffuse"), 5);
        if (g_table_roughness_tex) {
            glActiveTexture(GL_TEXTURE6);
            glBindTexture(GL_TEXTURE_2D, g_table_roughness_tex);
            glUniform1i(glGetUniformLocation(g_program, "uClockRoughnessTex"), 6);
        }
        if (g_table_metallic_tex) {
            glActiveTexture(GL_TEXTURE7);
            glBindTexture(GL_TEXTURE_2D, g_table_metallic_tex);
            glUniform1i(glGetUniformLocation(g_program, "uClockMetalnessTex"), 7);
        }
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(g_table_mesh.vao);
        glDrawArrays(GL_TRIANGLES, 0, g_table_mesh.count);
        glBindVertexArray(0);
        // Restore — the chessboard frame / squares / pieces draws
        // below all expect uClockTextureMode == 0.
        glUniform1i(glGetUniformLocation(g_program, "uClockTextureMode"), 0);
        glUniform1i(glGetUniformLocation(g_program, "uClockPbrMapsMode"), 0);
    }

    // Board
    float bnm[9]; mat4_normal_matrix(board_model, bnm);
    glUniformMatrix4fv(glGetUniformLocation(g_program, "uModel"), 1, GL_FALSE, board_model.m);
    glUniformMatrix3fv(glGetUniformLocation(g_program, "uNormalMat"), 1, GL_FALSE, bnm);
    // --- Squares: flat lacquered colors + a screen-space sample
    // of the planar-reflection texture. Roughness held at ~0.08
    // so the IBL env term still gives ambient sheen, while the
    // reflection texture (the prior mirrored-camera pass)
    // delivers the actual piece silhouettes.
    glUniform1i(glGetUniformLocation(g_program, "uWoodTextureMode"), 0);

    if (reflection_ready) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, g_reflection_color_tex);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(glGetUniformLocation(g_program, "uPlanarReflectionMode"), 1);
        glUniform1i(glGetUniformLocation(g_program, "uReflectionTex"), 4);
        float vp_w = static_cast<float>(width);
        float vp_h = static_cast<float>(height);
        glUniform2f(glGetUniformLocation(g_program, "uViewportSize"),
                    vp_w, vp_h);
    } else {
        glUniform1i(glGetUniformLocation(g_program, "uPlanarReflectionMode"), 0);
    }

    // Light squares: warm cream lacquer, restrained reflection so
    // the pieces' silhouettes hint at the surface without
    // reading as a literal mirror (was 0.85, dropped to 0.55).
    glUniform1f(glGetUniformLocation(g_program, "uReflectionStrength"), 0.55f);
    glBindVertexArray(g_board_squares_light_vao);
    set_material(g_program, 0.78f, 0.70f, 0.50f, 0.0f, 0.08f, 1.0f, 0);
    glDrawArrays(GL_TRIANGLES, 0, g_board_squares_light_count);

    // Dark squares: near-pure-black albedo + tamer reflection so
    // they actually read as black instead of grey-from-reflection.
    // Roughness bumped a touch (0.08 → 0.18) so the surface sheen
    // is softer too, which sells the matte-black feel. Reflection
    // dialed back a touch more (was 0.35) to match the lighter
    // touch on the cream squares.
    glUniform1f(glGetUniformLocation(g_program, "uReflectionStrength"), 0.22f);
    glBindVertexArray(g_board_squares_dark_vao);
    set_material(g_program, 0.015f, 0.012f, 0.010f, 0.0f, 0.18f, 1.0f, 0);
    glDrawArrays(GL_TRIANGLES, 0, g_board_squares_dark_count);

    // Squares done — clear the planar-reflection mode so the
    // frame and pieces below don't sample the reflection texture
    // by accident.
    glUniform1i(glGetUniformLocation(g_program, "uPlanarReflectionMode"), 0);

    // --- Walnut frame: triplanar-sample the diffuse + specular
    // textures the model ships when both are loaded; fall back
    // to the shader's procedural wood grain (uWoodEffect=1) if
    // either map is missing (file-not-found, sandbox, etc.).
    if (g_wood_diffuse_tex && g_wood_specular_tex) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, g_wood_diffuse_tex);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, g_wood_specular_tex);
        // Texture unit 0 stays bound to the shadow map, which
        // the main pass relies on; leave it alone.
        glActiveTexture(GL_TEXTURE0);

        glUniform1i(glGetUniformLocation(g_program, "uWoodTextureMode"), 1);
        glUniform1i(glGetUniformLocation(g_program, "uWoodDiffuse"), 2);
        glUniform1i(glGetUniformLocation(g_program, "uWoodSpecular"), 3);
        // ~3 tiles per project unit — tweak for denser grain.
        glUniform1f(glGetUniformLocation(g_program, "uWoodScale"), 0.35f);

        glBindVertexArray(g_board_frame_vao);
        // uAlbedo doubles as a tint multiplier on the texture
        // result; (1,1,1) shows the texture untinted.
        set_material(g_program, 1.0f, 1.0f, 1.0f, 0.0f, 0.30f, 1.0f, 0);
        glDrawArrays(GL_TRIANGLES, 0, g_board_frame_count);

        // Reset so the piece draws below don't sample the wood
        // texture by accident.
        glUniform1i(glGetUniformLocation(g_program, "uWoodTextureMode"), 0);
    } else {
        glBindVertexArray(g_board_frame_vao);
        set_material(g_program, 0.32f, 0.18f, 0.10f, 0.0f, 0.20f, 1.0f, 1);
        glDrawArrays(GL_TRIANGLES, 0, g_board_frame_count);
    }

    // --- Silver lining: pure-white metallic strip that runs
    // along the frame's inner edge, restoring the chrome detail
    // from the original Sketchfab model (Material.006/.007).
    // metallic=1.0 + low roughness gives a polished chrome read;
    // the IBL env-map term (sampleEnvironment) tints the
    // reflection by the four-light studio rig so it picks up the
    // warm-cream highlight from above.
    if (g_board_lining_count > 0) {
        glBindVertexArray(g_board_lining_vao);
        set_material(g_program, 0.95f, 0.95f, 0.95f,
                     /*metallic=*/1.0f,
                     /*roughness=*/0.18f,
                     /*ao=*/1.0f,
                     /*wood=*/0);
        glDrawArrays(GL_TRIANGLES, 0, g_board_lining_count);
    }
    glBindVertexArray(0);

    // ----- 3D chess clock — sits alongside the right side of the
    // board with its long base parallel to the board's edge.
    //
    //   * The mesh's local origin has Y=0 on its base and is
    //     centred on X/Z, with long axis along X (3 units),
    //     height along Y (1.3 units), depth along Z (0.95 units).
    //   * Rotating +90° around Y swings the long axis to lie
    //     along world Z (parallel to the board's right edge) and
    //     swings the dial face to point in the -X direction —
    //     toward the chessboard, as the player at either end of
    //     the board would expect.
    //   * Translating to X = +5.7 keeps the clock close to the
    //     board's right rank — the bezel ends ~0.3 game units
    //     from the frame, comfortably alongside without touching.
    //   * Y = -0.608 drops the clock's base flush with the bottom
    //     of the walnut frame (the frame STL spans Y from -0.608
    //     to -0.020, so the clock sits on the same notional table
    //     surface as the board instead of floating on the play
    //     plane).
    if (g_clock_body.count > 0) {
        Mat4 clock_model = mat4_multiply(
            mat4_translate(5.7f, -0.608f, 0.0f),
            mat4_rotate_y(-static_cast<float>(M_PI) * 0.5f));
        float cnm[9]; mat4_normal_matrix(clock_model, cnm);
        GLint loc_model    = glGetUniformLocation(g_program, "uModel");
        GLint loc_normal   = glGetUniformLocation(g_program, "uNormalMat");
        GLint loc_wood     = glGetUniformLocation(g_program, "uWoodTextureMode");
        GLint loc_opacity  = glGetUniformLocation(g_program, "uMaterialOpacity");
        glUniformMatrix4fv(loc_model,  1, GL_FALSE, clock_model.m);
        glUniformMatrix3fv(loc_normal, 1, GL_FALSE, cnm);
        glUniform1i(loc_wood, 0);

        // Body — UV-mapped diffuse + roughness + metalness maps
        // from the bundled Sketchfab textures. uAlbedo stays
        // (1,1,1) so the texture comes through unmodified.
        GLint loc_clock_mode      = glGetUniformLocation(g_program, "uClockTextureMode");
        GLint loc_clock_diff      = glGetUniformLocation(g_program, "uClockDiffuse");
        GLint loc_clock_rough     = glGetUniformLocation(g_program, "uClockRoughnessTex");
        GLint loc_clock_metal     = glGetUniformLocation(g_program, "uClockMetalnessTex");
        GLint loc_clock_pbr_mode  = glGetUniformLocation(g_program, "uClockPbrMapsMode");
        glUniform1f(loc_opacity, 1.0f);
        glUniform1i(loc_clock_mode, 1);
        glUniform1i(loc_clock_pbr_mode, 1);
        if (g_clock_diffuse_tex) {
            glActiveTexture(GL_TEXTURE5);
            glBindTexture(GL_TEXTURE_2D, g_clock_diffuse_tex);
            glUniform1i(loc_clock_diff, 5);
        }
        if (g_clock_roughness_tex) {
            glActiveTexture(GL_TEXTURE6);
            glBindTexture(GL_TEXTURE_2D, g_clock_roughness_tex);
            glUniform1i(loc_clock_rough, 6);
        }
        if (g_clock_metalness_tex) {
            glActiveTexture(GL_TEXTURE7);
            glBindTexture(GL_TEXTURE_2D, g_clock_metalness_tex);
            glUniform1i(loc_clock_metal, 7);
        }
        glActiveTexture(GL_TEXTURE0);
        set_material(g_program,
                     /*r=*/1.0f, /*g=*/1.0f, /*b=*/1.0f,
                     /*metallic=*/1.0f,
                     /*roughness=*/1.0f,
                     /*ao=*/1.0f,
                     /*wood=*/0);
        glBindVertexArray(g_clock_body.vao);
        glDrawArrays(GL_TRIANGLES, 0, g_clock_body.count);

        // Dial faces — flat circular meshes (Cursore.dx/sx in
        // the source, the inner discs visible behind the glass).
        // Use clock_cursor.png as the diffuse — that's the
        // texture with the numbers + knight icon. PBR maps off
        // here since the dial is a uniform matte surface.
        if (g_clock_cursor_tex) {
            glActiveTexture(GL_TEXTURE5);
            glBindTexture(GL_TEXTURE_2D, g_clock_cursor_tex);
            glUniform1i(loc_clock_diff, 5);
            glActiveTexture(GL_TEXTURE0);
        }
        glUniform1i(loc_clock_pbr_mode, 0);
        set_material(g_program,
                     /*r=*/1.0f, /*g=*/1.0f, /*b=*/1.0f,
                     /*metallic=*/0.0f,
                     /*roughness=*/0.55f,
                     /*ao=*/1.0f,
                     /*wood=*/0);
        // Dial faces draw STATIC with the body matrix — they're
        // the textured discs (numbers + knight) the user reads,
        // not the moving parts.
        if (g_clock_dial_r.count > 0) {
            glBindVertexArray(g_clock_dial_r.vao);
            glDrawArrays(GL_TRIANGLES, 0, g_clock_dial_r.count);
        }
        if (g_clock_dial_l.count > 0) {
            glBindVertexArray(g_clock_dial_l.vao);
            glDrawArrays(GL_TRIANGLES, 0, g_clock_dial_l.count);
        }
        // Reset cursor texture mode so the needle draws below pick
        // up the body's PBR maps (chrome / dark metal look) rather
        // than the dial-face cursor texture.
        glUniform1i(loc_clock_mode, 0);
        glUniform1i(loc_clock_pbr_mode, 0);

        // Needles — each rotates around its own pivot in the dial
        // face's plane (local Z axis). Right dial = white's clock,
        // left = black's. Long minute hands tick at 1 rev / minute
        // for visibility; short sub-dial hands at 1/12 of that
        // (1 rev / 12 minutes), mirroring an analog watch's
        // minute-vs-hour hand ratio (long fast, short slow).
        const float ang_long_white  = dial_angle_rad(white_thought_ms);
        const float ang_long_black  = dial_angle_rad(black_thought_ms);
        const float ang_short_white = ang_long_white  * (1.0f / 12.0f);
        const float ang_short_black = ang_long_black  * (1.0f / 12.0f);
        auto hand_model = [&](const float pv[3], float angle) -> Mat4 {
            Mat4 m = mat4_multiply(clock_model,
                                   mat4_translate(pv[0], pv[1], pv[2]));
            m = mat4_multiply(m, mat4_rotate_z(angle));
            m = mat4_multiply(m, mat4_translate(-pv[0], -pv[1], -pv[2]));
            return m;
        };
        auto draw_hand = [&](ClockMesh& mesh, const float pv[3],
                             float angle) {
            if (mesh.count <= 0) return;
            Mat4 m = hand_model(pv, angle);
            float nm[9]; mat4_normal_matrix(m, nm);
            glUniformMatrix4fv(loc_model,  1, GL_FALSE, m.m);
            glUniformMatrix3fv(loc_normal, 1, GL_FALSE, nm);
            glBindVertexArray(mesh.vao);
            glDrawArrays(GL_TRIANGLES, 0, mesh.count);
        };
        // Needles use the body's PBR maps for a polished metal
        // read. Rebind clock_diffuse to TEXTURE5 — the dial-face
        // block above swapped it for clock_cursor.
        if (g_clock_diffuse_tex) {
            glActiveTexture(GL_TEXTURE5);
            glBindTexture(GL_TEXTURE_2D, g_clock_diffuse_tex);
            glUniform1i(loc_clock_diff, 5);
            glActiveTexture(GL_TEXTURE0);
        }
        glUniform1i(loc_clock_pbr_mode, 1);
        set_material(g_program,
                     /*r=*/1.0f, /*g=*/1.0f, /*b=*/1.0f,
                     /*metallic=*/1.0f,
                     /*roughness=*/0.35f,
                     /*ao=*/1.0f,
                     /*wood=*/0);
        // Left dial = white's, right dial = black's. (Determined
        // empirically — the clock sits at world X+5.7 with a
        // rotate_y(-π/2) so its mesh-local +X dial maps to world
        // Z+0.6 and -X to Z-0.6; whichever side white sits on in
        // the camera frame, the visible-active needle has to fall
        // on the player-on-move's side, which is the LEFT dial here.)
        draw_hand(g_clock_hand_long_l,  CLOCK_HAND_LONG_L_PIVOT,  ang_long_white);
        draw_hand(g_clock_hand_short_l, CLOCK_HAND_SHORT_L_PIVOT, ang_short_white);
        draw_hand(g_clock_hand_long_r,  CLOCK_HAND_LONG_R_PIVOT,  ang_long_black);
        draw_hand(g_clock_hand_short_r, CLOCK_HAND_SHORT_R_PIVOT, ang_short_black);
        // Silver press-down levers. Each side translates along Y
        // by (blend - 1) × press_distance, so blend=1.0 (UP) leaves
        // it where the source mesh has it, and blend=0.0 (DOWN)
        // sinks the cap into the body. Left lever = white (matches
        // the dial assignment). PBR maps stay enabled so the
        // chrome detailing on the cap reads as polished metal.
        const float yL = (white_lever_blend - 1.0f) * CLOCK_LEVER_PRESS;
        const float yR = (black_lever_blend - 1.0f) * CLOCK_LEVER_PRESS;
        if (g_clock_lever_l.count > 0) {
            Mat4 m = mat4_multiply(clock_model,
                                   mat4_translate(0.0f, yL, 0.0f));
            float nm[9]; mat4_normal_matrix(m, nm);
            glUniformMatrix4fv(loc_model,  1, GL_FALSE, m.m);
            glUniformMatrix3fv(loc_normal, 1, GL_FALSE, nm);
            glBindVertexArray(g_clock_lever_l.vao);
            glDrawArrays(GL_TRIANGLES, 0, g_clock_lever_l.count);
        }
        if (g_clock_lever_r.count > 0) {
            Mat4 m = mat4_multiply(clock_model,
                                   mat4_translate(0.0f, yR, 0.0f));
            float nm[9]; mat4_normal_matrix(m, nm);
            glUniformMatrix4fv(loc_model,  1, GL_FALSE, m.m);
            glUniformMatrix3fv(loc_normal, 1, GL_FALSE, nm);
            glBindVertexArray(g_clock_lever_r.vao);
            glDrawArrays(GL_TRIANGLES, 0, g_clock_lever_r.count);
        }
        // Restore body matrix + PBR-mode so the glass dial draws
        // below transform with the clock body again.
        glUniformMatrix4fv(loc_model,  1, GL_FALSE, clock_model.m);
        glUniformMatrix3fv(loc_normal, 1, GL_FALSE, cnm);
        glUniform1i(loc_clock_pbr_mode, 0);

        // Glass dials — transparent + low roughness so the studio
        // rig glints off them. Drawn last with GL_BLEND on so the
        // body + needles already in the depth buffer show through.
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glUniform1f(loc_opacity, 0.28f);
        set_material(g_program,
                     /*r=*/0.78f, /*g=*/0.78f, /*b=*/0.80f,
                     /*metallic=*/0.0f,
                     /*roughness=*/0.05f,
                     /*ao=*/1.0f,
                     /*wood=*/0);
        if (g_clock_glass_r.count > 0) {
            glBindVertexArray(g_clock_glass_r.vao);
            glDrawArrays(GL_TRIANGLES, 0, g_clock_glass_r.count);
        }
        if (g_clock_glass_l.count > 0) {
            glBindVertexArray(g_clock_glass_l.vao);
            glDrawArrays(GL_TRIANGLES, 0, g_clock_glass_l.count);
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glUniform1f(loc_opacity, 1.0f);
        glBindVertexArray(0);
    }

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

        // Animate whichever piece is at the from-square — colour-
        // agnostic. Was previously gated on !bp.is_white because
        // the original design assumed AI always plays black, but
        // the same animation now also drives sensor-driven moves
        // (any colour) and AI-as-white when the human picks black.
        bool animating = gs.ai_animating && bp.col == gs.ai_from_col && bp.row == gs.ai_from_row;
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
            // Material picks up the piece colour so a white piece
            // doesn't render black mid-flight.
            if (bp.is_white) set_material(g_program, 0.97f,0.95f,0.90f, 0,0.28f,1, 0);
            else             set_material(g_program, 0.02f,0.02f,0.02f, 0,0.35f,1, 0);
            draw_with_model(g_program, pm, g_pieces[bp.type].vao, g_pieces[bp.type].num_vertices);
            continue;
        }

        Mat4 pm = piece_model_matrix(wx, wz, s, bp.is_white, rot_z_to_y);
        if (bp.is_white) set_material(g_program, 0.97f,0.95f,0.90f, 0,0.28f,1, 0);
        else set_material(g_program, 0.02f,0.02f,0.02f, 0,0.35f,1, 0);
        draw_with_model(g_program, pm, g_pieces[bp.type].vao, g_pieces[bp.type].num_vertices);
    }

    // "Why?" ghost move — when a flagged-move panel is open and the
    // board is rewound to that move's pre-move position, draw a
    // translucent cyan ghost of the engine's recommended piece at its
    // destination, plus an arrow from where the piece sits now. Shows
    // "this is the move you should have played" on the right board.
    if (gs.why_ply >= 1 && gs.analysis_mode &&
        gs.analysis_index == gs.why_ply - 1 &&
        gs.why_ply < static_cast<int>(gs.best_move.size()) &&
        gs.best_move[gs.why_ply].size() >= 4) {
        // Internal col mirrors the file (file_to_internal_col = 7-file);
        // the row is rank-1. Matches parse_uci_move + the grid.
        const std::string& uci = gs.best_move[gs.why_ply];
        int fc = 7 - (uci[0] - 'a'), fr = uci[1] - '1';
        int tc = 7 - (uci[2] - 'a'), tr = uci[3] - '1';
        if (in_bounds(fc, fr) && in_bounds(tc, tr)) {
            int idx = gs.grid[fr][fc];
            if (idx >= 0 && idx < static_cast<int>(gs.pieces.size())) {
                const BoardPiece& mp = gs.pieces[idx];
                float wx, wz; square_center(tc, tr, wx, wz);
                float s = BASE_PIECE_SCALE * piece_scale[mp.type];
                Mat4 pm = piece_model_matrix(wx, wz, s, mp.is_white, rot_z_to_y);

                // Translucent bright-cyan ghost of the moving piece at the
                // destination (g_program is still bound from the piece
                // loop above). High opacity so it clearly reads.
                glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);
                glUniform1f(glGetUniformLocation(g_program, "uMaterialOpacity"), 0.78f);
                set_material(g_program, 0.45f, 0.92f, 1.0f, 0, 0.22f, 1, 0);
                draw_with_model(g_program, pm, g_pieces[mp.type].vao,
                                g_pieces[mp.type].num_vertices);
                glUniform1f(glGetUniformLocation(g_program, "uMaterialOpacity"), 1.0f);
                glDepthMask(GL_TRUE); glDisable(GL_BLEND);

                // Board-space arrow drawer — reused for the best move
                // (bright cyan) and the engine's expected reply (amber).
                glUseProgram(g_highlight_program);
                glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);
                glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"), 1, GL_FALSE, vp.m);
                glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
                glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
                float ay = BOARD_Y + 0.02f;
                auto draw_board_arrow = [&](int afc, int afr, int atc, int atr,
                                            float cr, float cg, float cb, float ca) {
                    float fx, fz, txc, tzc;
                    square_center(afc, afr, fx, fz);
                    square_center(atc, atr, txc, tzc);
                    float dx = txc-fx, dz = tzc-fz;
                    float len = std::sqrt(dx*dx+dz*dz);
                    if (len <= 1e-3f) return;
                    float nx = -dz/len*0.075f, nz = dx/len*0.075f;
                    float hl = 0.20f, hw = 0.17f;
                    float hx = txc - dx/len*hl, hz = tzc - dz/len*hl;
                    float hnx = -dz/len*hw, hnz = dx/len*hw;
                    std::vector<float> av = {
                        fx+nx,ay,fz+nz, fx-nx,ay,fz-nz, hx+nx,ay,hz+nz,
                        fx-nx,ay,fz-nz, hx-nx,ay,hz-nz, hx+nx,ay,hz+nz,
                        hx+hnx,ay,hz+hnz, hx-hnx,ay,hz-hnz, txc,ay,tzc
                    };
                    GLuint avao, avbo;
                    glGenVertexArrays(1, &avao); glGenBuffers(1, &avbo);
                    glBindVertexArray(avao); glBindBuffer(GL_ARRAY_BUFFER, avbo);
                    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(av.size()*sizeof(float)), av.data(), GL_STREAM_DRAW);
                    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
                    glEnableVertexAttribArray(0);
                    glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), cr,cg,cb,ca);
                    glDrawArrays(GL_TRIANGLES, 0, 9);
                    glBindVertexArray(0); glDeleteBuffers(1, &avbo); glDeleteVertexArrays(1, &avao);
                };
                // Your best move (cyan).
                draw_board_arrow(fc, fr, tc, tr, 0.30f, 0.92f, 1.0f, 0.95f);
                // The engine's expected reply — second move of the PV.
                // Drawn from the opponent piece's current square (it sits
                // there in this rewound position), amber + a touch dimmer
                // so it reads as "…and they answer".
                if (gs.why_ply < static_cast<int>(gs.best_pv.size())) {
                    const std::string& pv = gs.best_pv[gs.why_ply];
                    size_t sp = pv.find(' ');
                    if (sp != std::string::npos && pv.size() >= sp + 5) {
                        const char* r = pv.c_str() + sp + 1;
                        int rfc = 7 - (r[0]-'a'), rfr = r[1]-'1';
                        int rtc = 7 - (r[2]-'a'), rtr = r[3]-'1';
                        if (in_bounds(rfc, rfr) && in_bounds(rtc, rtr))
                            draw_board_arrow(rfc, rfr, rtc, rtr,
                                             1.0f, 0.58f, 0.16f, 0.78f);
                    }
                }
                glDepthMask(GL_TRUE); glDisable(GL_BLEND);
                glUseProgram(g_program);
            }
        }
    }

    // Captured pieces. piece_model_matrix puts the piece's bottom on
    // the BOARD playing surface (Y=BOARD_Y), but captured pieces are
    // displayed off the board, alongside the chessboard frame, where
    // the table is the actual surface they should rest on. Shift the
    // bottom from BOARD_Y down to the table-top plane.
    {
        constexpr float TABLE_TOP_Y = -0.608f;
        // Captured pieces sit on the -X side (opposite the clock at
        // +X) on the side of the player whose colour they are. Each
        // colour gets a 2-col × 8-row strip = 16 slots — more than
        // the 15 captures that are theoretically possible (every
        // piece except the king).
        //
        // Z layout — table spans Z ∈ [-7, +7]. We anchor each
        // colour's far row at Z = ±5.7, leaving a ~1-unit table
        // buffer on each end and a ~1-unit gap between the two
        // colour boxes. World footprint of the largest piece (rook
        // at full scale) is ~0.22 radius, so the slot centres stay
        // ≈0.5 inside every box edge.
        //
        //   X cols (both colours): -5.2 (closer to board), -6.2
        //   White rows: Z = -5.7, -5.0, -4.3, -3.6, -2.9, -2.2,
        //       -1.5, -0.8
        //   Black rows: Z = 5.7, 5.0, 4.3, 3.6, 2.9, 2.2, 1.5, 0.8
        constexpr int   CAP_COLS        = 2;
        constexpr int   CAP_ROWS        = 8;
        constexpr float CAP_X_SPACING   = 1.0f;
        constexpr float CAP_Z_SPACING   = 0.7f;
        constexpr float CAP_X0          = -5.2f;
        constexpr float CAP_WHITE_Z0    = -5.7f;
        constexpr float CAP_BLACK_Z0    =  5.7f;
        constexpr int   CAP_MAX_SLOTS   = CAP_COLS * CAP_ROWS;

        int wc = 0, bc = 0;
        for (const auto& bp : gs.pieces) {
            if (bp.alive) continue;
            float s = BASE_PIECE_SCALE * piece_scale[bp.type];
            int& cnt = bp.is_white ? wc : bc;
            int local = cnt < CAP_MAX_SLOTS ? cnt : CAP_MAX_SLOTS - 1;
            int ri = local / CAP_COLS;
            int ci = local % CAP_COLS;
            float px = CAP_X0 - ci * CAP_X_SPACING;
            float pz = bp.is_white ? (CAP_WHITE_Z0 + ri * CAP_Z_SPACING)
                                   : (CAP_BLACK_Z0 - ri * CAP_Z_SPACING);
            Mat4 pm = piece_model_matrix(px, pz, s, bp.is_white, rot_z_to_y);
            // Drop the piece so its bottom rests on the table.
            pm = mat4_multiply(
                mat4_translate(0.0f, TABLE_TOP_Y - BOARD_Y, 0.0f), pm);
            if (bp.is_white) set_material(g_program, 0.85f,0.82f,0.74f, 0,0.4f,0.7f, 0);
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

    // Square-control heatmap (C key, analysis/review only): flat per-
    // square tints — blue = only white attacks it, red = only black,
    // purple = contested. Drawn under the pieces (depth-tested) so it
    // reads as board paint. Gated on analysis_mode so it can't appear
    // during a normal live game.
    if (gs.show_control && gs.analysis_mode) {
        std::vector<float> vw, vb, vc;   // white-control / black / contested
        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < 8; c++) {
                bool wa = is_square_attacked(gs, c, r, true);
                bool ba = is_square_attacked(gs, c, r, false);
                if (!wa && !ba) continue;
                std::vector<float>& dst = (wa && ba) ? vc : (wa ? vw : vb);
                float sx, sz; square_center(c, r, sx, sz);
                const float e = 0.46f;
                const float q[18] = {
                    sx-e,hy,sz-e, sx+e,hy,sz-e, sx+e,hy,sz+e,
                    sx-e,hy,sz-e, sx+e,hy,sz+e, sx-e,hy,sz+e
                };
                dst.insert(dst.end(), q, q + 18);
            }
        }
        glUniformMatrix4fv(lmvp, 1, GL_FALSE, vp.m);
        glUniform1f(lin, 0.0f); glUniform1f(lout, 0.0f);   // flat-fill mode
        glUniform1i(glGetUniformLocation(g_highlight_program, "uUseGradient"), 0);
        glUniform1i(glGetUniformLocation(g_highlight_program, "uUseVertexColor"), 0);
        auto draw_tint = [&](std::vector<float>& v, float cr, float cg, float cb) {
            if (v.empty()) return;
            GLuint vao, vbo; glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
            glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(v.size()*sizeof(float)), v.data(), GL_STREAM_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
            glEnableVertexAttribArray(0);
            glUniform4f(lcol_loc, cr, cg, cb, 0.36f);
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(v.size()/3));
            glBindVertexArray(0); glDeleteBuffers(1, &vbo); glDeleteVertexArrays(1, &vao);
        };
        draw_tint(vw, 0.20f, 0.48f, 0.95f);   // white controls — blue
        draw_tint(vb, 0.95f, 0.30f, 0.25f);   // black controls — red
        draw_tint(vc, 0.72f, 0.36f, 0.82f);   // contested — purple
    }

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

    // Move-hint rings — yellow disc on the source + destination of
    // Stockfish's recommended move when Options → Move hints is on.
    // Same disc primitive as the valid-move rings above; different
    // colour and slightly larger inner/outer radii so the source
    // square reads distinctly from a click-selected piece's blue
    // glow above.
    if (gs.hint_from_col >= 0 && gs.hint_to_col >= 0) {
        float hi = 0.30f + pulse * 0.06f;
        float ho = 0.46f + pulse * 0.04f;
        float ha = 0.55f + pulse * 0.25f;
        // From-square: filled yellow ring.
        {
            float sx, sz;
            square_center(gs.hint_from_col, gs.hint_from_row, sx, sz);
            Mat4 mvp = mat4_multiply(vp, mat4_translate(sx, hy, sz));
            glUniformMatrix4fv(lmvp, 1, GL_FALSE, mvp.m);
            glUniform4f(lcol_loc, 1.0f, 0.85f, 0.15f, ha);
            glUniform1f(lin, hi); glUniform1f(lout, ho);
            glBindVertexArray(g_disc_vao);
            glDrawArrays(GL_TRIANGLES, 0, g_disc_vertex_count);
            glBindVertexArray(0);
        }
        // To-square: same yellow.
        {
            float dx, dz;
            square_center(gs.hint_to_col, gs.hint_to_row, dx, dz);
            Mat4 mvp = mat4_multiply(vp, mat4_translate(dx, hy, dz));
            glUniformMatrix4fv(lmvp, 1, GL_FALSE, mvp.m);
            glUniform4f(lcol_loc, 1.0f, 0.85f, 0.15f, ha);
            glUniform1f(lin, hi); glUniform1f(lout, ho);
            glBindVertexArray(g_disc_vao);
            glDrawArrays(GL_TRIANGLES, 0, g_disc_vertex_count);
            glBindVertexArray(0);
        }
    }
    glDepthMask(GL_TRUE); glDisable(GL_BLEND);

    // --- Board coordinate labels (a-h, 1-8): faux-engraved on wood.
    // The etched shader uses the same font alpha texture but adds
    // soft inner shadow + a thin lit/shadow rim along the +Z/-Z edges
    // of each glyph so the labels read as carved into the walnut
    // frame instead of painted on top. Atlas dims (768 × 288) come
    // from text_atlas.cpp's CELL_SIZE × ATLAS_COLS / ATLAS_ROWS; the
    // texel size is precomputed for the rim taps.
    {
        glUseProgram(g_etched_label_program);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_font_tex);
        glUniform1i(glGetUniformLocation(g_etched_label_program, "uFontTex"), 0);
        glUniformMatrix4fv(glGetUniformLocation(g_etched_label_program, "uMVP"),
                           1, GL_FALSE, vp.m);
        glUniform2f(glGetUniformLocation(g_etched_label_program, "uTexelSize"),
                    1.0f / 768.0f, 1.0f / 288.0f);
        // Very dark walnut stain for the body — keep it warm so it
        // doesn't read as blue/grey ink.
        glUniform3f(glGetUniformLocation(g_etched_label_program, "uEtchTint"),
                    0.04f, 0.025f, 0.012f);
        // Lit rim catches the warm cream of the key light bouncing
        // off the upward-facing wall of the engraved groove.
        glUniform3f(glGetUniformLocation(g_etched_label_program, "uHighlight"),
                    0.85f, 0.75f, 0.55f);
        // Shadow rim is just deep brown — barely visible, but pushes
        // the impression that the glyph dips below the surface.
        glUniform3f(glGetUniformLocation(g_etched_label_program, "uShadowRim"),
                    0.02f, 0.012f, 0.006f);
        glUniform1f(glGetUniformLocation(g_etched_label_program, "uOpacity"),
                    0.85f);

        glBindVertexArray(g_label_vao);
        glDrawArrays(GL_TRIANGLES, 0, g_label_vertex_count);
        glBindVertexArray(0);

        glDepthMask(GL_TRUE); glDisable(GL_BLEND);
    }

    // --- MSAA resolve + (optional) cartoon-outline post-process ---
    // Desktop: 3D wrote into the MS FBO; resolve to either the
    // single-sample scene FBO (outline) or default FB (no outline).
    // Web: 3D wrote directly to scene_fbo / default FB, just run
    // the post-process if needed (no resolve).
    //
    // Multi-game flow: the offscreen FBOs are sized to (width, height)
    // — the sub-viewport's dims — but the destination is the default
    // FB at sub-rect (sub_x, sub_y, width, height).
#ifdef __EMSCRIPTEN__
    // Web wrote straight into the default FB at
    // (sub_x, sub_y, width, height) — nothing to resolve.
#else
    resolve_scene_ms_to(static_cast<GLuint>(default_fbo),
                        sub_x, sub_y,
                        width, height, /*include_depth=*/false);
#endif

    // After the resolve / post-process, all subsequent NDC overlay
    // draws should land inside the sub-viewport on the default FB.
    glViewport(sub_x, sub_y, width, height);

    draw_score_graph(gs, human_plays_white);
    draw_move_list(gs);
    draw_why_panel(gs, width, height);

    if (draw_clock) {
        draw_clock_widget(gs, clock_ms_remaining, clock_side_is_white);
    }

    if (draw_flag && flag != nullptr && !flag->p.empty()) {
        draw_withdraw_flag_widget(flag, width, height);
    }

    if (withdraw_confirm_open) {
        draw_withdraw_confirm_modal(withdraw_hover, withdraw_confirm_title);
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
                        bool chessnut_connected) {
    // Button layout (BTN_*) defined in menu_input.h so it stays in
    // sync with menu_hit_test's click regions.
    using namespace menu_ui;

    GLint default_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &default_fbo);

    // 3D pass renders into the multisample scene FBO on desktop so
    // menu pieces tumbling against the dark backdrop get free MSAA;
    // resolve before the UI overlay. On web the browser canvas
    // already does MSAA for free and the MS FBO has format-
    // compatibility issues with the WebGL default FB, so fall back
    // to the original direct-render path.
#ifndef __EMSCRIPTEN__
    ensure_scene_ms_fbo(width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, g_scene_ms_fbo);
#endif

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glViewport(0, 0, width, height);

    float aspect = static_cast<float>(width) / static_cast<float>(height);
    float deg2rad = static_cast<float>(M_PI) / 180.0f;

    float cam_angle = time * 15.0f, cam_pitch = 25.0f, cam_dist = 12.0f;
    Mat4 view = mat4_multiply(mat4_translate(0, 0, -cam_dist),
        mat4_multiply(mat4_rotate_x(cam_pitch * deg2rad), mat4_rotate_y(cam_angle * deg2rad)));
    Mat4 proj = mat4_perspective(45.0f * deg2rad, aspect, 0.1f, 250.0f);

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

#ifndef __EMSCRIPTEN__
    resolve_scene_ms_to(static_cast<GLuint>(default_fbo),
                        0, 0, width, height, /*include_depth=*/false);
#endif

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
    const float start_y     = btn_start_y(chessnut_connected);
    const float challenge_y = btn_challenge_y(chessnut_connected);
    const float puzzle_y    = btn_puzzle_y(chessnut_connected);
    const float options_y   = btn_options_y(chessnut_connected);
#ifndef __EMSCRIPTEN__
    const float quit_y      = btn_quit_y(chessnut_connected);
#endif
    int multi_end = subtitle_end;
    if (chessnut_connected) {
        std::string mp_text = "Multiplayer";
        float mtw = mp_text.size() * bcw * 0.7f;
        add_screen_string(ui_verts, -mtw*0.5f,
                          btn_multiplayer_y() - 0.018f,
                          bcw, bch, mp_text);
        multi_end = static_cast<int>(ui_verts.size() / 5);
    }
    std::string start_text = "Start Game";
    float stw = start_text.size() * bcw * 0.7f;
    add_screen_string(ui_verts, -stw*0.5f, start_y - 0.018f, bcw, bch, start_text);
    int start_end = static_cast<int>(ui_verts.size() / 5);

    std::string ch_text = "Practice";
    // "Practice" comfortably fits BTN_W = 0.35 NDC at the regular
    // button cell, so it keeps the same bcw/bch as Start Game.
    float chw = ch_text.size() * bcw * 0.7f;
    add_screen_string(ui_verts, -chw*0.5f, challenge_y - 0.018f,
                      bcw, bch, ch_text);
    int ch_end = static_cast<int>(ui_verts.size() / 5);

    std::string puz_text = "Puzzles";
    float pzw = puz_text.size() * bcw * 0.7f;
    add_screen_string(ui_verts, -pzw*0.5f, puzzle_y - 0.018f, bcw, bch, puz_text);
    int puz_end = static_cast<int>(ui_verts.size() / 5);

    std::string opt_text = "Options";
    float otw = opt_text.size() * bcw * 0.7f;
    add_screen_string(ui_verts, -otw*0.5f, options_y - 0.018f, bcw, bch, opt_text);
    int opt_end = static_cast<int>(ui_verts.size() / 5);

#ifndef __EMSCRIPTEN__
    std::string quit_text = "Quit";
    float qtw = quit_text.size() * bcw * 0.7f;
    add_screen_string(ui_verts, -qtw*0.5f, quit_y - 0.018f, bcw, bch, quit_text);
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

    // Button backgrounds — chamfered walnut blocks shared with all
    // other UI screens via render_internal::draw_wood_button.
    if (chessnut_connected)
        draw_wood_button(BTN_X, btn_multiplayer_y(), BTN_W, BTN_H,
                         hover_button == 5);
    draw_wood_button(BTN_X, start_y,     BTN_W, BTN_H, hover_button == 1);
    draw_wood_button(BTN_X, challenge_y, BTN_W, BTN_H, hover_button == 3);
    draw_wood_button(BTN_X, puzzle_y,    BTN_W, BTN_H, hover_button == 6);
    draw_wood_button(BTN_X, options_y,   BTN_W, BTN_H, hover_button == 4);
#ifndef __EMSCRIPTEN__
    draw_wood_button(BTN_X, quit_y,      BTN_W, BTN_H, hover_button == 2);
#endif

    glUseProgram(g_text_program);
    glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"), 1, GL_FALSE, id.m);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);
    glBindVertexArray(uvao);

    // Title + subtitle: bind the Cinzel atlas (display face) so the
    // brand identity stays Roman-inscriptional. Soft gold matches
    // the rest of the warm palette.
    glBindTexture(GL_TEXTURE_2D, g_title_font_tex);
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                0.95f, 0.84f, 0.50f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, title_count);
    if (subtitle_end > title_count) {
        glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                    0.88f, 0.82f, 0.65f, 0.85f);
        glDrawArrays(GL_TRIANGLES, title_count, subtitle_end - title_count);
    }
    // Switch to the body atlas (Inter) for the menu options — the
    // sans face is cleaner at small sizes and pairs well with the
    // serif title.
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    // All button labels share one gold palette — hover lifts the
    // luminance a touch (1.0 vs 0.85) instead of swapping hues.
    auto label_color = [&](bool hovered) {
        float k = hovered ? 1.00f : 0.85f;
        glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                    0.95f * k, 0.84f * k, 0.55f * k, 1.0f);
    };
    if (multi_end > subtitle_end) {
        label_color(hover_button == 5);
        glDrawArrays(GL_TRIANGLES, subtitle_end, multi_end - subtitle_end);
    }
    label_color(hover_button == 1);
    glDrawArrays(GL_TRIANGLES, multi_end, start_end - multi_end);
    label_color(hover_button == 3);
    glDrawArrays(GL_TRIANGLES, start_end, ch_end - start_end);
    label_color(hover_button == 6);
    glDrawArrays(GL_TRIANGLES, ch_end, puz_end - ch_end);
    label_color(hover_button == 4);
    glDrawArrays(GL_TRIANGLES, puz_end, opt_end - puz_end);
#ifndef __EMSCRIPTEN__
    label_color(hover_button == 2);
    glDrawArrays(GL_TRIANGLES, opt_end, quit_end - opt_end);
#endif

    glBindVertexArray(0); glDeleteBuffers(1, &uvbo); glDeleteVertexArrays(1, &uvao);
    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}


