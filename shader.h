#pragma once

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

// GLSL shader sources (definitions in shader.cpp)
extern const char* vertex_shader_src;
extern const char* fragment_shader_src;
extern const char* shadow_vs_src;
extern const char* shadow_fs_src;
extern const char* jelly_exit_fs_src;
extern const char* jelly_glass_fs_src;
extern const char* highlight_vs_src;
extern const char* highlight_fs_src;
extern const char* text_vs_src;
extern const char* text_fs_src;
extern const char* etched_vs_src;
extern const char* etched_fs_src;
extern const char* wood_button_vs_src;
extern const char* wood_button_fs_src;
extern const char* shatter_vs_src;
extern const char* shatter_fs_src;
extern const char* skybox_vs_src;
extern const char* skybox_fs_src;
extern const char* splat_vs_src;
extern const char* splat_fs_src;
extern const char* splat_blit_vs_src;
extern const char* splat_blit_fs_src;
extern const char* splat_depth_blit_fs_src;

GLuint compile_shader(GLenum type, const char* src);
GLuint create_program(const char* vs_src, const char* fs_src);
