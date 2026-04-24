#include "vec.h"

#include <cmath>

float dot(const Vec3& a, const Vec3& b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}

float length(const Vec3& a) {
    return std::sqrt(dot(a, a));
}

Vec3 normalize(const Vec3& a) {
    float len = length(a);
    if (len < 1e-20f) return {0, 0, 0};
    return {a.x/len, a.y/len, a.z/len};
}
