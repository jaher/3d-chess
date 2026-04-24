#pragma once

#include "vec.h"  // Vec4 for mat4_mul_vec4

// Column-major 4x4 matrix used for every view/projection/model
// transform. Storage matches OpenGL's convention so we can hand
// m[] straight to glUniformMatrix4fv with transpose=GL_FALSE.
struct Mat4 {
    float m[16];
};

Mat4 mat4_identity();
Mat4 mat4_multiply(const Mat4& a, const Mat4& b);
Mat4 mat4_perspective(float fovy_rad, float aspect, float near_val, float far_val);
Mat4 mat4_translate(float x, float y, float z);
Mat4 mat4_rotate_x(float rad);
Mat4 mat4_rotate_y(float rad);
Mat4 mat4_rotate_z(float rad);
Mat4 mat4_scale(float x, float y, float z);
Mat4 mat4_ortho(float left, float right, float bottom, float top,
                float near_val, float far_val);
Mat4 mat4_look_at(float ex, float ey, float ez,
                  float cx, float cy, float cz,
                  float ux, float uy, float uz);
Mat4 mat4_inverse(const Mat4& mat);

Vec4 mat4_mul_vec4(const Mat4& m, const Vec4& v);

// Extract the upper-left 3x3 of a column-major Mat4 into a 3x3
// float array (the normal matrix the shaders want).
void mat4_normal_matrix(const Mat4& model, float out[9]);
