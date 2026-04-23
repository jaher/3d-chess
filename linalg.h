#pragma once

struct Mat4 {
    float m[16]; // column-major
};

struct Vec3 {
    float x, y, z;
    Vec3 operator+(const Vec3& o) const { return {x+o.x, y+o.y, z+o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x-o.x, y-o.y, z-o.z}; }
    Vec3 operator*(float s) const { return {x*s, y*s, z*s}; }
};

float dot(const Vec3& a, const Vec3& b);
float length(const Vec3& a);
Vec3 normalize(const Vec3& a);

struct Vec4 {
    float x, y, z, w;
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

// Extract upper-left 3x3 of a column-major 4x4 into a 3x3 array (for normal matrix)
void mat4_normal_matrix(const Mat4& model, float out[9]);
