// Tests for the column-major 4x4 matrix math used by the renderer
// and the screen-to-board picking code in app_state.cpp.

#include "doctest.h"

#include "../mat.h"

#include <cmath>

// Compare two Mat4s element-wise with a small tolerance. doctest has
// its own doctest::Approx but we spell out the check ourselves to get
// useful failure messages.
static bool mat4_near(const Mat4& a, const Mat4& b, float eps = 1e-4f) {
    for (int i = 0; i < 16; i++)
        if (std::abs(a.m[i] - b.m[i]) > eps) return false;
    return true;
}

// ---------------------------------------------------------------------------
TEST_CASE("mat4_identity: diagonal ones, off-diagonal zeros") {
    Mat4 I = mat4_identity();
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            float expected = (c == r) ? 1.0f : 0.0f;
            CHECK(I.m[c*4 + r] == doctest::Approx(expected));
        }
    }
}

TEST_CASE("mat4_identity * M == M") {
    Mat4 I = mat4_identity();
    Mat4 M = mat4_multiply(
        mat4_translate(1.0f, 2.0f, 3.0f),
        mat4_rotate_y(0.5f));
    CHECK(mat4_near(mat4_multiply(I, M), M));
    CHECK(mat4_near(mat4_multiply(M, I), M));
}

TEST_CASE("mat4_mul_vec4: identity leaves vectors unchanged") {
    Mat4 I = mat4_identity();
    Vec4 v{1.0f, 2.0f, 3.0f, 1.0f};
    Vec4 r = mat4_mul_vec4(I, v);
    CHECK(r.x == doctest::Approx(v.x));
    CHECK(r.y == doctest::Approx(v.y));
    CHECK(r.z == doctest::Approx(v.z));
    CHECK(r.w == doctest::Approx(v.w));
}

TEST_CASE("mat4_translate moves a point") {
    Mat4 T = mat4_translate(3.0f, 4.0f, 5.0f);
    Vec4 p{0.0f, 0.0f, 0.0f, 1.0f};
    Vec4 r = mat4_mul_vec4(T, p);
    CHECK(r.x == doctest::Approx(3.0f));
    CHECK(r.y == doctest::Approx(4.0f));
    CHECK(r.z == doctest::Approx(5.0f));
    CHECK(r.w == doctest::Approx(1.0f));
}

TEST_CASE("mat4_rotate_x(0) is the identity") {
    Mat4 R = mat4_rotate_x(0.0f);
    CHECK(mat4_near(R, mat4_identity()));
}

TEST_CASE("mat4_rotate_y(PI) negates x and z") {
    Mat4 R = mat4_rotate_y(static_cast<float>(M_PI));
    Vec4 p{1.0f, 2.0f, 3.0f, 1.0f};
    Vec4 r = mat4_mul_vec4(R, p);
    CHECK(r.x == doctest::Approx(-1.0f));
    CHECK(r.y == doctest::Approx( 2.0f));
    CHECK(r.z == doctest::Approx(-3.0f));
}

TEST_CASE("mat4_inverse: M * inverse(M) is approximately identity") {
    // Build a non-trivial matrix similar to the composed view matrix
    // used by the renderer and screen_to_board.
    float deg2rad = static_cast<float>(M_PI) / 180.0f;
    Mat4 view = mat4_multiply(
        mat4_translate(0, 0, -12.0f),
        mat4_multiply(mat4_rotate_x(30.0f * deg2rad),
                      mat4_multiply(mat4_rotate_y(180.0f * deg2rad),
                                    mat4_translate(0, 0, 0))));
    Mat4 inv = mat4_inverse(view);
    CHECK(mat4_near(mat4_multiply(view, inv), mat4_identity(), 1e-4f));
    CHECK(mat4_near(mat4_multiply(inv, view), mat4_identity(), 1e-4f));
}

TEST_CASE("mat4_scale scales each axis independently") {
    Mat4 S = mat4_scale(2.0f, 3.0f, 4.0f);
    Vec4 p{1.0f, 1.0f, 1.0f, 1.0f};
    Vec4 r = mat4_mul_vec4(S, p);
    CHECK(r.x == doctest::Approx(2.0f));
    CHECK(r.y == doctest::Approx(3.0f));
    CHECK(r.z == doctest::Approx(4.0f));
}
