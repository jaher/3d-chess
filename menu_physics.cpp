#include "menu_physics.h"

#include "linalg.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>

namespace {

// World-space half-extents of each piece's normalised-object-space
// bounding box, populated once by menu_physics_init. The boundary-
// collision path reads this directly for a rotated AABB; the piece-
// piece path uses the finer sub-box stack below instead.
float g_piece_half_extent[PIECE_COUNT][3] = {};

// Per-piece collision decomposition. A single bounding box inflates
// to the piece's widest part (the base), leaving empty air around
// the narrower head. Splitting the mesh into a stack of horizontal
// slices keeps each region's box hugged to the geometry at that
// height, so two kings meeting tip-to-tip don't collide through the
// empty space above their crowns.
constexpr int MENU_PIECE_SUBBOXES = 4;
struct PieceSubBox {
    float cx, cy, cz;  // local-space centre (normalised object space)
    float ex, ey, ez;  // local-space half-extents
};
PieceSubBox g_piece_subboxes[PIECE_COUNT][MENU_PIECE_SUBBOXES] = {};

// SAT-based OBB-vs-OBB test (Ericson, "Real-Time Collision
// Detection", §4.4.1). On overlap, returns the minimum translation
// vector as a unit normal (pointing from A toward B) plus its depth.
// The 9 edge-edge cross-product axes participate in the separation
// test; only the 6 face axes feed into the contact normal, because
// an edge-edge MTV gives awkward sliding response on an impulse
// model and the face normals are plenty for the menu bounce.
bool obb_overlap(const float ca[3], const float ua[3][3], const float ea[3],
                 const float cb[3], const float ub[3][3], const float eb[3],
                 float& n_x, float& n_y, float& n_z, float& depth) {
    constexpr float EPS = 1e-6f;

    // R[i][j] = ua[i] · ub[j]; AbsR = |R| + eps to keep parallel
    // edges from producing spurious zero-length cross-product axes.
    float R[3][3], AbsR[3][3];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            R[i][j] = ua[i][0]*ub[j][0] + ua[i][1]*ub[j][1] + ua[i][2]*ub[j][2];
            AbsR[i][j] = std::fabs(R[i][j]) + EPS;
        }
    }

    // Translation from a's centre to b's, expressed in a's frame.
    float t_w[3] = {cb[0]-ca[0], cb[1]-ca[1], cb[2]-ca[2]};
    float t[3] = {
        t_w[0]*ua[0][0] + t_w[1]*ua[0][1] + t_w[2]*ua[0][2],
        t_w[0]*ua[1][0] + t_w[1]*ua[1][1] + t_w[2]*ua[1][2],
        t_w[0]*ua[2][0] + t_w[1]*ua[2][1] + t_w[2]*ua[2][2],
    };

    float min_pen = std::numeric_limits<float>::max();
    float best_nx = 0.0f, best_ny = 0.0f, best_nz = 0.0f;
    bool has_axis = false;

    auto take_face_axis = [&](float wx, float wy, float wz, float pen, float sign) {
        if (pen < min_pen) {
            min_pen = pen;
            best_nx = wx * sign;
            best_ny = wy * sign;
            best_nz = wz * sign;
            has_axis = true;
        }
    };

    for (int i = 0; i < 3; i++) {
        float ra = ea[i];
        float rb = eb[0]*AbsR[i][0] + eb[1]*AbsR[i][1] + eb[2]*AbsR[i][2];
        float overlap = (ra + rb) - std::fabs(t[i]);
        if (overlap < 0.0f) return false;
        float sign = (t[i] < 0.0f) ? -1.0f : 1.0f;
        take_face_axis(ua[i][0], ua[i][1], ua[i][2], overlap, sign);
    }

    for (int i = 0; i < 3; i++) {
        float ra = ea[0]*AbsR[0][i] + ea[1]*AbsR[1][i] + ea[2]*AbsR[2][i];
        float rb = eb[i];
        float tproj = t[0]*R[0][i] + t[1]*R[1][i] + t[2]*R[2][i];
        float overlap = (ra + rb) - std::fabs(tproj);
        if (overlap < 0.0f) return false;
        float sign = (tproj < 0.0f) ? -1.0f : 1.0f;
        take_face_axis(ub[i][0], ub[i][1], ub[i][2], overlap, sign);
    }

    // 9 edge-edge separation checks (no contribution to MTV).
    // Standard Ericson formulas; each cross-product's projection
    // length can be read off from AbsR without computing the cross.
    {
        float ra = ea[1]*AbsR[2][0] + ea[2]*AbsR[1][0];
        float rb = eb[1]*AbsR[0][2] + eb[2]*AbsR[0][1];
        if (std::fabs(t[2]*R[1][0] - t[1]*R[2][0]) > ra + rb) return false;
    }
    {
        float ra = ea[1]*AbsR[2][1] + ea[2]*AbsR[1][1];
        float rb = eb[0]*AbsR[0][2] + eb[2]*AbsR[0][0];
        if (std::fabs(t[2]*R[1][1] - t[1]*R[2][1]) > ra + rb) return false;
    }
    {
        float ra = ea[1]*AbsR[2][2] + ea[2]*AbsR[1][2];
        float rb = eb[0]*AbsR[0][1] + eb[1]*AbsR[0][0];
        if (std::fabs(t[2]*R[1][2] - t[1]*R[2][2]) > ra + rb) return false;
    }
    {
        float ra = ea[0]*AbsR[2][0] + ea[2]*AbsR[0][0];
        float rb = eb[1]*AbsR[1][2] + eb[2]*AbsR[1][1];
        if (std::fabs(t[0]*R[2][0] - t[2]*R[0][0]) > ra + rb) return false;
    }
    {
        float ra = ea[0]*AbsR[2][1] + ea[2]*AbsR[0][1];
        float rb = eb[0]*AbsR[1][2] + eb[2]*AbsR[1][0];
        if (std::fabs(t[0]*R[2][1] - t[2]*R[0][1]) > ra + rb) return false;
    }
    {
        float ra = ea[0]*AbsR[2][2] + ea[2]*AbsR[0][2];
        float rb = eb[0]*AbsR[1][1] + eb[1]*AbsR[1][0];
        if (std::fabs(t[0]*R[2][2] - t[2]*R[0][2]) > ra + rb) return false;
    }
    {
        float ra = ea[0]*AbsR[1][0] + ea[1]*AbsR[0][0];
        float rb = eb[1]*AbsR[2][2] + eb[2]*AbsR[2][1];
        if (std::fabs(t[1]*R[0][0] - t[0]*R[1][0]) > ra + rb) return false;
    }
    {
        float ra = ea[0]*AbsR[1][1] + ea[1]*AbsR[0][1];
        float rb = eb[0]*AbsR[2][2] + eb[2]*AbsR[2][0];
        if (std::fabs(t[1]*R[0][1] - t[0]*R[1][1]) > ra + rb) return false;
    }
    {
        float ra = ea[0]*AbsR[1][2] + ea[1]*AbsR[0][2];
        float rb = eb[0]*AbsR[2][1] + eb[1]*AbsR[2][0];
        if (std::fabs(t[1]*R[0][2] - t[0]*R[1][2]) > ra + rb) return false;
    }

    if (!has_axis) return false;
    depth = min_pen;
    n_x = best_nx; n_y = best_ny; n_z = best_nz;
    return true;
}

}  // namespace

void menu_physics_init(StlModel loaded_models[PIECE_COUNT]) {
    for (int t = 0; t < PIECE_COUNT; ++t) {
        const BoundingBox& bb = loaded_models[t].bounding_box();
        float max_ext = bb.max_extent();
        if (max_ext < 1e-6f) {
            g_piece_half_extent[t][0] = 1.0f;
            g_piece_half_extent[t][1] = 1.0f;
            g_piece_half_extent[t][2] = 1.0f;
            continue;
        }
        float norm_scale = 2.0f / max_ext;
        Vertex c = bb.center();
        g_piece_half_extent[t][0] = (bb.max.x - c.x) * norm_scale;
        g_piece_half_extent[t][1] = (bb.max.y - c.y) * norm_scale;
        g_piece_half_extent[t][2] = (bb.max.z - c.z) * norm_scale;

        // Per-slice collision boxes. Each slice spans a uniform band
        // of the piece's local Y range; x/z extents come from the
        // actual vertex spread inside that band, so a narrow head
        // produces a narrow sub-box even though the base is wide.
        const StlModel& stl = loaded_models[t];
        float y_lo_n = (bb.min.y - c.y) * norm_scale;
        float y_hi_n = (bb.max.y - c.y) * norm_scale;
        float span   = y_hi_n - y_lo_n;
        for (int s = 0; s < MENU_PIECE_SUBBOXES; s++) {
            float slice_lo = y_lo_n +
                span * static_cast<float>(s)     / MENU_PIECE_SUBBOXES;
            float slice_hi = y_lo_n +
                span * static_cast<float>(s + 1) / MENU_PIECE_SUBBOXES;

            float xmin = 1e30f, xmax = -1e30f;
            float zmin = 1e30f, zmax = -1e30f;
            bool any = false;
            for (const auto& tri : stl.triangles()) {
                const Vertex* verts[3] = {&tri.v0, &tri.v1, &tri.v2};
                for (int i = 0; i < 3; i++) {
                    float vy = (verts[i]->y - c.y) * norm_scale;
                    if (vy < slice_lo || vy > slice_hi) continue;
                    float vx = (verts[i]->x - c.x) * norm_scale;
                    float vz = (verts[i]->z - c.z) * norm_scale;
                    if (vx < xmin) xmin = vx;
                    if (vx > xmax) xmax = vx;
                    if (vz < zmin) zmin = vz;
                    if (vz > zmax) zmax = vz;
                    any = true;
                }
            }

            PieceSubBox& pb = g_piece_subboxes[t][s];
            if (!any) {
                pb.cx = pb.cy = pb.cz = 0.0f;
                pb.ex = pb.ey = pb.ez = 0.0f;
                continue;
            }
            pb.cx = (xmin + xmax) * 0.5f;
            pb.cy = (slice_lo + slice_hi) * 0.5f;
            pb.cz = (zmin + zmax) * 0.5f;
            pb.ex = (xmax - xmin) * 0.5f;
            pb.ey = (slice_hi - slice_lo) * 0.5f;
            pb.ez = (zmax - zmin) * 0.5f;
        }
    }
}

void menu_init_physics(std::vector<PhysicsPiece>& pieces) {
    pieces.clear();

    // Wall-clock seed so each menu entry looks different. Local
    // mt19937 so we don't touch global rand state.
    const auto seed = static_cast<uint32_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::mt19937 rng(seed);
    auto rf = [&](float lo, float hi) {
        std::uniform_real_distribution<float> d(lo, hi);
        return d(rng);
    };

    std::array<PieceType, 12> types = {
        KING, QUEEN, BISHOP, KNIGHT, ROOK, PAWN,
        KING, QUEEN, BISHOP, KNIGHT, ROOK, PAWN,
    };
    std::shuffle(types.begin(), types.end(), rng);

    for (int i = 0; i < 12; i++) {
        PhysicsPiece p;
        p.type = types[i];
        p.x = rf(-4.5f, 4.5f);
        p.z = rf(-4.5f, 4.5f);
        // Waterfall stagger with per-piece jitter so entry doesn't
        // look mechanical.
        p.y = 3.0f + static_cast<float>(i) * 1.4f + rf(-0.4f, 0.4f);
        p.vx = rf(-1.8f, 1.8f);
        p.vy = rf(-0.5f, 0.5f);
        p.vz = rf(-1.8f, 1.8f);
        p.rot_x = rf(0.0f, 360.0f);
        p.rot_y = rf(0.0f, 360.0f);
        p.rot_z = rf(0.0f, 360.0f);
        p.spin_x = rf(-80.0f, 80.0f);
        p.spin_y = rf(-80.0f, 80.0f);
        p.spin_z = rf(-60.0f, 60.0f);
        // Scale buckets picked randomly instead of i%3 so size
        // variety doesn't correlate with stagger position.
        static constexpr float scale_bucket[3] = {0.35f, 0.45f, 0.55f};
        std::uniform_int_distribution<int> pick(0, 2);
        p.scale = scale_bucket[pick(rng)];
        pieces.push_back(p);
    }
}

// Standard "abs-of-rotation" trick (Ericson §4.2.6): for rotation
// matrix R and local half extents h, the world AABB half extents
// are H.x = |R[0][0]|*h.x + |R[0][1]|*h.y + |R[0][2]|*h.z and
// similarly for y and z. Cheaper than transforming the 8 corners.
void menu_piece_world_half_extents(const PhysicsPiece& p, float out[3]) {
    const float deg2rad = static_cast<float>(M_PI) / 180.0f;
    const float s = BASE_PIECE_SCALE * piece_scale[p.type] * p.scale / 0.35f;

    const float hx_loc = g_piece_half_extent[p.type][0] * s;
    const float hy_loc = g_piece_half_extent[p.type][1] * s;
    const float hz_loc = g_piece_half_extent[p.type][2] * s;

    // Rotation order matches renderer_draw_menu: R = Rz * Ry * Rx.
    Mat4 rot = mat4_multiply(
        mat4_rotate_z(p.rot_z * deg2rad),
        mat4_multiply(mat4_rotate_y(p.rot_y * deg2rad),
                      mat4_rotate_x(p.rot_x * deg2rad)));
    // Extract the 3x3 from the column-major Mat4.
    const float r00 = rot.m[0], r01 = rot.m[4], r02 = rot.m[8];
    const float r10 = rot.m[1], r11 = rot.m[5], r12 = rot.m[9];
    const float r20 = rot.m[2], r21 = rot.m[6], r22 = rot.m[10];

    out[0] = std::fabs(r00) * hx_loc +
             std::fabs(r01) * hy_loc +
             std::fabs(r02) * hz_loc;
    out[1] = std::fabs(r10) * hx_loc +
             std::fabs(r11) * hy_loc +
             std::fabs(r12) * hz_loc;
    out[2] = std::fabs(r20) * hx_loc +
             std::fabs(r21) * hy_loc +
             std::fabs(r22) * hz_loc;
}

void menu_update_physics(std::vector<PhysicsPiece>& pieces, float dt) {
    const float gravity = -9.8f;
    const float floor_y = -2.0f;
    const float bounce = 0.6f;
    const float wall = 6.0f;
    const float damping = 0.998f;

    // --- Integrate (gravity + ballistic motion + spin) ---
    for (auto& p : pieces) {
        p.vy += gravity * dt;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.z += p.vz * dt;
        p.rot_x += p.spin_x * dt;
        p.rot_y += p.spin_y * dt;
        p.rot_z += p.spin_z * dt;
    }

    // --- World boundary collisions using each piece's rotated
    //     world-space AABB so pieces sit flat on the floor at their
    //     base and bounce off the walls at their silhouette. ---
    for (auto& p : pieces) {
        float h[3];
        menu_piece_world_half_extents(p, h);

        if (p.y - h[1] < floor_y) {
            p.y = floor_y + h[1];
            p.vy = std::abs(p.vy) * bounce;
            p.spin_x *= 0.8f; p.spin_y *= 0.8f; p.spin_z *= 0.8f;
            p.spin_x += (p.vx > 0 ? 1.0f : -1.0f) * 20.0f;
        }
        if (p.x - h[0] < -wall) {
            p.x = -wall + h[0];
            p.vx = std::abs(p.vx) * bounce;
        }
        if (p.x + h[0] > wall) {
            p.x = wall - h[0];
            p.vx = -std::abs(p.vx) * bounce;
        }
        if (p.z - h[2] < -wall) {
            p.z = -wall + h[2];
            p.vz = std::abs(p.vz) * bounce;
        }
        if (p.z + h[2] > wall) {
            p.z = wall - h[2];
            p.vz = -std::abs(p.vz) * bounce;
        }
        p.vx *= damping;
        p.vz *= damping;
    }

    // --- Piece-piece collisions via stacked-OBB overlap ---
    //
    // Each piece is approximated by MENU_PIECE_SUBBOXES thin
    // vertical slice OBBs rigidly attached to its transform. Pair
    // collision iterates every slice of A against every slice of B;
    // the deepest overlap wins and drives a single positional
    // correction + impulse response for the pair.
    const int n = static_cast<int>(pieces.size());

    // Precompute each piece's rotation-matrix columns and scale once;
    // those are shared by every sub-box on that piece.
    std::vector<std::array<std::array<float, 3>, 3>> axes(n);
    std::vector<float> piece_scales(n);
    std::vector<std::array<float, 3>> piece_centres(n);
    for (int i = 0; i < n; ++i) {
        const PhysicsPiece& p = pieces[i];
        const float deg2rad = static_cast<float>(M_PI) / 180.0f;
        const float s = BASE_PIECE_SCALE * piece_scale[p.type] * p.scale / 0.35f;
        piece_scales[i] = s;
        piece_centres[i][0] = p.x;
        piece_centres[i][1] = p.y;
        piece_centres[i][2] = p.z;
        Mat4 rot = mat4_multiply(
            mat4_rotate_z(p.rot_z * deg2rad),
            mat4_multiply(mat4_rotate_y(p.rot_y * deg2rad),
                          mat4_rotate_x(p.rot_x * deg2rad)));
        for (int k = 0; k < 3; ++k) {
            axes[i][k][0] = rot.m[k * 4 + 0];
            axes[i][k][1] = rot.m[k * 4 + 1];
            axes[i][k][2] = rot.m[k * 4 + 2];
        }
    }

    auto subbox_world_centre = [&](int piece_index, int subbox_index,
                                   float out[3]) {
        const PieceSubBox& pb =
            g_piece_subboxes[pieces[piece_index].type][subbox_index];
        float s = piece_scales[piece_index];
        float lx = pb.cx * s, ly = pb.cy * s, lz = pb.cz * s;
        const auto& u = axes[piece_index];
        out[0] = piece_centres[piece_index][0]
               + u[0][0]*lx + u[1][0]*ly + u[2][0]*lz;
        out[1] = piece_centres[piece_index][1]
               + u[0][1]*lx + u[1][1]*ly + u[2][1]*lz;
        out[2] = piece_centres[piece_index][2]
               + u[0][2]*lx + u[1][2]*ly + u[2][2]*lz;
    };

    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            PhysicsPiece& a = pieces[i];
            PhysicsPiece& b = pieces[j];

            float ua[3][3], ub[3][3];
            for (int k = 0; k < 3; ++k) {
                ua[k][0] = axes[i][k][0]; ua[k][1] = axes[i][k][1]; ua[k][2] = axes[i][k][2];
                ub[k][0] = axes[j][k][0]; ub[k][1] = axes[j][k][1]; ub[k][2] = axes[j][k][2];
            }

            float best_nx = 0.0f, best_ny = 0.0f, best_nz = 0.0f;
            float best_depth = -1.0f;

            for (int sa = 0; sa < MENU_PIECE_SUBBOXES; ++sa) {
                const PieceSubBox& boxa = g_piece_subboxes[a.type][sa];
                float ea[3] = {boxa.ex * piece_scales[i],
                               boxa.ey * piece_scales[i],
                               boxa.ez * piece_scales[i]};
                if (ea[0] <= 0.0f || ea[1] <= 0.0f || ea[2] <= 0.0f) continue;
                float ca[3];
                subbox_world_centre(i, sa, ca);

                for (int sb = 0; sb < MENU_PIECE_SUBBOXES; ++sb) {
                    const PieceSubBox& boxb = g_piece_subboxes[b.type][sb];
                    float eb[3] = {boxb.ex * piece_scales[j],
                                   boxb.ey * piece_scales[j],
                                   boxb.ez * piece_scales[j]};
                    if (eb[0] <= 0.0f || eb[1] <= 0.0f || eb[2] <= 0.0f) continue;
                    float cb[3];
                    subbox_world_centre(j, sb, cb);

                    float nx, ny, nz, depth;
                    if (!obb_overlap(ca, ua, ea, cb, ub, eb, nx, ny, nz, depth)) continue;
                    if (depth > best_depth) {
                        best_depth = depth;
                        best_nx = nx; best_ny = ny; best_nz = nz;
                    }
                }
            }

            if (best_depth < 0.0f) continue;

            const float half_d = best_depth * 0.5f;
            a.x -= best_nx * half_d;
            a.y -= best_ny * half_d;
            a.z -= best_nz * half_d;
            b.x += best_nx * half_d;
            b.y += best_ny * half_d;
            b.z += best_nz * half_d;

            // Positive rv_n means A and B are approaching along the
            // contact normal; flip that component with 0.85 restitution.
            const float rv_n = (a.vx - b.vx) * best_nx +
                               (a.vy - b.vy) * best_ny +
                               (a.vz - b.vz) * best_nz;
            if (rv_n > 0.0f) {
                const float imp = rv_n * 0.85f;
                a.vx -= imp * best_nx;
                a.vy -= imp * best_ny;
                a.vz -= imp * best_nz;
                b.vx += imp * best_nx;
                b.vy += imp * best_ny;
                b.vz += imp * best_nz;
            }
        }
    }
}
