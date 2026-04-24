#pragma once

// Minimal vector types used across the renderer + physics + picking.
// Separate from mat.h so modules that only need vector math don't
// pull in the full Mat4 surface.

struct Vec3 {
    float x, y, z;
    Vec3 operator+(const Vec3& o) const { return {x+o.x, y+o.y, z+o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x-o.x, y-o.y, z-o.z}; }
    Vec3 operator*(float s) const { return {x*s, y*s, z*s}; }
};

struct Vec4 {
    float x, y, z, w;
};

float dot(const Vec3& a, const Vec3& b);
float length(const Vec3& a);
Vec3 normalize(const Vec3& a);
