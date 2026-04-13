#pragma once

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#else
#include <epoxy/gl.h>
#endif

// GLSL shader sources (definitions in shader.cpp)
extern const char* vertex_shader_src;
extern const char* fragment_shader_src;
extern const char* shadow_vs_src;
extern const char* shadow_fs_src;
extern const char* highlight_vs_src;
extern const char* highlight_fs_src;
extern const char* text_vs_src;
extern const char* text_fs_src;
extern const char* shatter_vs_src;
extern const char* shatter_fs_src;

GLuint compile_shader(GLenum type, const char* src);
GLuint create_program(const char* vs_src, const char* fs_src);
