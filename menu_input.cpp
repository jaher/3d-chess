#include "menu_input.h"

#include "mat.h"
#include "menu_physics.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <utility>

namespace {

// Rebuild the menu camera's view/projection + derive a world-space
// ray from a screen pixel. Must stay in sync with the view matrix
// constructed in renderer_draw_menu.
bool menu_screen_ray(double mx, double my, int width, int height,
                     float time, float ro[3], float rd[3]) {
    float deg2rad = static_cast<float>(M_PI) / 180.0f;
    float aspect = static_cast<float>(width) / static_cast<float>(height);
    float cam_angle = time * 15.0f, cam_pitch = 25.0f, cam_dist = 12.0f;
    Mat4 view = mat4_multiply(
        mat4_translate(0, 0, -cam_dist),
        mat4_multiply(mat4_rotate_x(cam_pitch * deg2rad),
                      mat4_rotate_y(cam_angle * deg2rad)));
    Mat4 proj = mat4_perspective(45.0f * deg2rad, aspect, 0.1f, 100.0f);
    Mat4 inv_vp = mat4_inverse(mat4_multiply(proj, view));

    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float ndc_y = 1.0f - 2.0f * static_cast<float>(my) / height;

    Vec4 nw = mat4_mul_vec4(inv_vp, {ndc_x, ndc_y, -1, 1});
    Vec4 fw = mat4_mul_vec4(inv_vp, {ndc_x, ndc_y,  1, 1});
    if (std::abs(nw.w) < 1e-10f || std::abs(fw.w) < 1e-10f) return false;

    ro[0] = nw.x / nw.w; ro[1] = nw.y / nw.w; ro[2] = nw.z / nw.w;
    float fx = fw.x / fw.w - ro[0];
    float fy = fw.y / fw.w - ro[1];
    float fz = fw.z / fw.w - ro[2];
    float len = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (len < 1e-10f) return false;
    rd[0] = fx / len; rd[1] = fy / len; rd[2] = fz / len;
    return true;
}

// Ray-vs-AABB slab test. Returns nearest positive t on hit, or <0 on miss.
float ray_aabb_t(const float ro[3], const float rd[3],
                 const float cmin[3], const float cmax[3]) {
    float tmin = -1e30f, tmax = 1e30f;
    for (int i = 0; i < 3; i++) {
        if (std::abs(rd[i]) < 1e-12f) {
            if (ro[i] < cmin[i] || ro[i] > cmax[i]) return -1.0f;
            continue;
        }
        float inv = 1.0f / rd[i];
        float t1 = (cmin[i] - ro[i]) * inv;
        float t2 = (cmax[i] - ro[i]) * inv;
        if (t1 > t2) std::swap(t1, t2);
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return -1.0f;
    }
    return tmin >= 0.0f ? tmin : (tmax >= 0.0f ? tmax : -1.0f);
}

}  // namespace

int menu_piece_hit_test(const std::vector<PhysicsPiece>& pieces,
                        double mx, double my, int width, int height,
                        float time) {
    float ro[3], rd[3];
    if (!menu_screen_ray(mx, my, width, height, time, ro, rd)) return -1;

    int best = -1;
    float best_t = 1e30f;
    for (size_t i = 0; i < pieces.size(); i++) {
        const PhysicsPiece& p = pieces[i];
        float h[3];
        menu_piece_world_half_extents(p, h);
        float cmin[3] = {p.x - h[0], p.y - h[1], p.z - h[2]};
        float cmax[3] = {p.x + h[0], p.y + h[1], p.z + h[2]};
        float t = ray_aabb_t(ro, rd, cmin, cmax);
        if (t >= 0.0f && t < best_t) {
            best_t = t;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void menu_throw_piece(PhysicsPiece& p,
                      double press_mx, double press_my,
                      double release_mx, double release_my,
                      float dt,
                      int width, int height, float time) {
    float ro_p[3], rd_p[3], ro_r[3], rd_r[3];
    if (!menu_screen_ray(press_mx, press_my, width, height, time, ro_p, rd_p)) return;
    if (!menu_screen_ray(release_mx, release_my, width, height, time, ro_r, rd_r)) return;

    // Project the piece's centre onto the press ray: this is the depth
    // at which we'll unproject both cursor positions, so the resulting
    // world delta tracks the on-screen drag at the piece's distance
    // from the camera.
    float t_depth = (p.x - ro_p[0]) * rd_p[0]
                  + (p.y - ro_p[1]) * rd_p[1]
                  + (p.z - ro_p[2]) * rd_p[2];
    if (t_depth < 1.0f) t_depth = 1.0f;

    float wp[3] = {ro_p[0] + rd_p[0] * t_depth,
                   ro_p[1] + rd_p[1] * t_depth,
                   ro_p[2] + rd_p[2] * t_depth};
    float wr[3] = {ro_r[0] + rd_r[0] * t_depth,
                   ro_r[1] + rd_r[1] * t_depth,
                   ro_r[2] + rd_r[2] * t_depth};

    // Clamp dt: sub-frame flicks would otherwise divide by near-zero
    // and launch pieces into the stratosphere. min_dt is generous
    // enough (~50 ms) to smooth out the fast touch-event streams that
    // mobile devices deliver — raw dt there is often ~4 ms, which
    // made flicks read absurdly fast on phones.
    constexpr float min_dt = 0.05f;
    if (dt < min_dt) dt = min_dt;
    constexpr float scale = 1.5f;
    float vx = (wr[0] - wp[0]) / dt * scale;
    float vy = (wr[1] - wp[1]) / dt * scale;
    float vz = (wr[2] - wp[2]) / dt * scale;

    // Cap peak throw speed so a stray fast swipe doesn't hurl a piece
    // clear across the scene before physics damping catches up.
    constexpr float MAX_SPEED = 18.0f;
    float v2 = vx * vx + vy * vy + vz * vz;
    if (v2 > MAX_SPEED * MAX_SPEED) {
        float k = MAX_SPEED / std::sqrt(v2);
        vx *= k; vy *= k; vz *= k;
    }

    p.vx += vx;
    p.vy += vy;
    p.vz += vz;
    p.vy += 2.0f;

    // Spin magnitude tracks throw speed so a sharp flick tumbles more
    // than a gentle shove.
    float speed = std::sqrt(vx * vx + vy * vy + vz * vz);
    const auto seed = static_cast<uint32_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);
    float spin_mag = 80.0f + speed * 40.0f;
    p.spin_x += d(rng) * spin_mag;
    p.spin_y += d(rng) * spin_mag;
    p.spin_z += d(rng) * spin_mag * 0.6f;
}

int menu_hit_test(double mx, double my, int width, int height) {
    using namespace menu_ui;
    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float ndc_y = 1.0f - 2.0f * static_cast<float>(my) / height;
    if (ndc_x < BTN_X || ndc_x > BTN_X + BTN_W) return 0;
    if (ndc_y >= BTN_START_Y     - BTN_H && ndc_y <= BTN_START_Y)     return 1;
    if (ndc_y >= BTN_CHALLENGE_Y - BTN_H && ndc_y <= BTN_CHALLENGE_Y) return 3;
    if (ndc_y >= BTN_OPTIONS_Y   - BTN_H && ndc_y <= BTN_OPTIONS_Y)   return 4;
#ifndef __EMSCRIPTEN__
    // No "Quit" button in the browser build — closing a tab from
    // inside a WASM app is awkward and not expected on the web.
    if (ndc_y >= BTN_QUIT_Y      - BTN_H && ndc_y <= BTN_QUIT_Y)      return 2;
#endif
    return 0;
}
