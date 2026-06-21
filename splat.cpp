#include "splat.h"

#include "compression.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

// Niantic SPZ legacy format (versions 1-3, gzip-framed).
// After ungzip the layout is:
//   bytes  0..3   : magic "NGSP"
//   bytes  4..7   : uint32 version
//   bytes  8..11  : uint32 vertex_count
//   byte   12     : uint8  shDegree (0..3)
//   byte   13     : uint8  fractionalBits (typ. 12)
//   byte   14     : uint8  flags
//   byte   15     : uint8  reserved
// Then six contiguous attribute blocks (in this exact order — the
// Niantic reference's `serializePackedGaussians`):
//   positions  : N × 9 bytes (3 axes, each a 24-bit signed fixed
//                point with `fractional_bits` bits after the binary
//                point)
//   alpha      : N × 1 byte  (uint8)
//   colors     : N × 3 bytes (uint8, encoded as
//                packed = (sh0 * 0.15 + 0.5) * 255 so the inverse is
//                sh0 = (packed/255 - 0.5) / 0.15, and the final RGB
//                is sh0 * 0.282 + 0.5 clamped to [0, 1])
//   scales     : N × 3 bytes (uint8, log-encoded; linear = exp(b/16-10).
//                ALL three axes kept — the EWA shader builds the 3D
//                covariance from anisotropic scales × rotation)
//   rotations  : VERSION-DEPENDENT width — decoded, NOT dropped (the
//                splats are anisotropic plates, so orientation matters):
//                  v1/v2: N × 3 bytes — xyz direct, (byte/127.5 - 1),
//                         w = +√(1 - x² - y² - z²).
//                  v3:    N × 4 bytes — smallest-three packing (top 2
//                         bits select the largest component; three
//                         10-bit fields give the rest).
//   sh         : N × extra (degree-dependent, skipped for sh=0):
//                degree 1 → 9 B, 2 → 24 B, 3 → 45 B.
// Per-splat = 19 bytes (v1/v2) or 20 bytes (v3) plus the SH tail.

namespace {

uint32_t read_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

// Decode one axis of the 24-bit signed fixed-point position. The
// 24 bits are stored little-endian, sign extended from bit 23 to
// the full int32.
int32_t read_i24(const uint8_t* p) {
    int32_t v = static_cast<int32_t>(p[0])
              | (static_cast<int32_t>(p[1]) << 8)
              | (static_cast<int32_t>(p[2]) << 16);
    if (v & 0x00800000) v |= ~0x00FFFFFF;     // sign-extend bit 23
    return v;
}

}  // namespace


std::vector<Splat> splat_load_spz(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        std::fprintf(stderr, "[splat] open failed: %s\n", path.c_str());
        return {};
    }
    auto end = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> raw(static_cast<std::size_t>(end));
    if (!in.read(reinterpret_cast<char*>(raw.data()),
                 static_cast<std::streamsize>(raw.size()))) {
        std::fprintf(stderr, "[splat] read failed: %s\n", path.c_str());
        return {};
    }

    std::vector<uint8_t> data;
    try {
        data = gunzip(raw.data(), raw.size());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[splat] gunzip failed: %s — %s\n",
                     path.c_str(), e.what());
        return {};
    }

    if (data.size() < 16
        || data[0] != 'N' || data[1] != 'G' || data[2] != 'S' || data[3] != 'P') {
        std::fprintf(stderr, "[splat] bad magic in %s\n", path.c_str());
        return {};
    }
    const uint32_t version   = read_u32(&data[4]);
    const uint32_t n         = read_u32(&data[8]);
    const uint8_t  sh_degree = data[12];
    const uint8_t  frac_bits = data[13];
    // data[14] = flags, data[15] = reserved — unused here.
    if (version > 3) {
        std::fprintf(stderr,
            "[splat] unsupported version %u in %s (need legacy v1-3)\n",
            version, path.c_str());
        return {};
    }
    // SH degree adds extra coefficients per splat. Each coeff is one
    // signed byte per RGB channel × per coefficient count. Degree 0
    // = 0 extra, 1 = 9, 2 = 24, 3 = 45.
    int sh_coeffs = 0;
    switch (sh_degree) {
        case 0: sh_coeffs =  0; break;
        case 1: sh_coeffs =  9; break;
        case 2: sh_coeffs = 24; break;
        case 3: sh_coeffs = 45; break;
        default: sh_coeffs = 0; break;
    }
    // Rotation block width is VERSION-DEPENDENT. v1/v2 store 3 bytes
    // (xyz components, direct); v3 stores 4 bytes (smallest-three
    // packing). Treating a v3 file as 3-byte rotations — as this
    // decoder used to — both mis-strides every splat AND feeds the
    // quaternion decoder garbage, so the (legitimately thin) plate
    // splats get random orientations and render as a spiky mess.
    const std::size_t rot_bytes = (version == 3) ? 4 : 3;
    const std::size_t per_splat =
        9 + 1 + 3 + 3 + rot_bytes + static_cast<std::size_t>(sh_coeffs);
    const std::size_t need = 16 + std::size_t(n) * per_splat;
    if (data.size() < need) {
        std::fprintf(stderr,
            "[splat] truncated SPZ in %s: have %zu need %zu\n",
            path.c_str(), data.size(), need);
        return {};
    }

    const uint8_t* base = data.data() + 16;
    const uint8_t* pos_blk    = base;
    const uint8_t* alpha_blk  = pos_blk    + std::size_t(n) * 9;
    const uint8_t* rgb_blk    = alpha_blk  + std::size_t(n) * 1;
    const uint8_t* scale_blk  = rgb_blk    + std::size_t(n) * 3;
    const uint8_t* rot_blk    = scale_blk  + std::size_t(n) * 3;

    const float frac_div = std::ldexp(1.0f, -static_cast<int>(frac_bits));

    std::vector<Splat> out;
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        Splat s;
        s.pos[0] = static_cast<float>(read_i24(pos_blk + std::size_t(i)*9 + 0)) * frac_div;
        s.pos[1] = static_cast<float>(read_i24(pos_blk + std::size_t(i)*9 + 3)) * frac_div;
        s.pos[2] = static_cast<float>(read_i24(pos_blk + std::size_t(i)*9 + 6)) * frac_div;
        // Niantic SPZ scale decode: linear = exp(packed/16 - 10).
        // Keep ALL three axes — the EWA shader builds the 3D
        // covariance from anisotropic scales × rotation.
        for (int k = 0; k < 3; ++k) {
            uint8_t b = scale_blk[std::size_t(i)*3 + k];
            s.scales[k] = std::exp(static_cast<float>(b) / 16.0f - 10.0f);
        }
        // Quaternion — encoding depends on SPZ version. Stay in double
        // (Splat.quat is double[4]); JS/Spark is double throughout, so
        // narrowing to float here drifts by 1 ULP.
        double qx, qy, qz, qw;
        if (version == 3) {
            // v3 smallest-three: 4 bytes → 32-bit LE word. Top 2 bits =
            // largestIndex (which quat component is largest); the
            // remaining 30 bits are three 10-bit fields (9-bit magnitude
            // + 1-bit sign) for the OTHER three components, each mapped
            // to (−1/√2, +1/√2). Largest component = +√(1 − Σ²). Spark
            // walks slots 3,2,1,0 — preserve that order or the bit
            // fields read out of phase.
            const uint8_t* q = rot_blk + std::size_t(i) * 4;
            const uint32_t w32 = static_cast<uint32_t>(q[0])
                               | (static_cast<uint32_t>(q[1]) << 8)
                               | (static_cast<uint32_t>(q[2]) << 16)
                               | (static_cast<uint32_t>(q[3]) << 24);
            const int      largest  = static_cast<int>(w32 >> 30);
            const double   max_val  = 1.0 / std::sqrt(2.0);
            const uint32_t mag_mask = (1u << 9) - 1u;
            double   comp[4] = {0, 0, 0, 0};
            uint32_t rem     = w32;
            double   sum_sq  = 0.0;
            for (int k = 3; k >= 0; --k) {
                if (k == largest) continue;
                uint32_t mag  = rem & mag_mask;
                uint32_t sign = (rem >> 9) & 1u;
                rem >>= 10;
                double v = max_val * (static_cast<double>(mag)
                                      / static_cast<double>(mag_mask));
                if (sign) v = -v;
                comp[k] = v;
                sum_sq += v * v;
            }
            comp[largest] = std::sqrt(std::max(0.0, 1.0 - sum_sq));
            qx = comp[0]; qy = comp[1]; qz = comp[2]; qw = comp[3];
        } else {
            // v1/v2: 3 bytes carrying (x, y, z), each unsigned byte
            // 0..255 mapped to (-1, +1) via `q = byte / 127.5 - 1`.
            // Reconstruct w = √(1 - x² - y² - z²). (Spark's unpack v2.)
            const uint8_t* q = rot_blk + std::size_t(i) * 3;
            qx = static_cast<double>(q[0]) / 127.5 - 1.0;
            qy = static_cast<double>(q[1]) / 127.5 - 1.0;
            qz = static_cast<double>(q[2]) / 127.5 - 1.0;
            qw = std::sqrt(std::max(0.0, 1.0 - qx*qx - qy*qy - qz*qz));
        }
        // Keep splats in raw SPZ-space here — the world transform
        // (Spark's `mesh.scale.set(1,-1,1)` decomposed-and-averaged
        // to R_z(180°) * uniformScale(1/3)) is applied in the model
        // matrix instead. Baking only a Y-flip here would make the
        // room left-right mirrored vs Spark, since Spark's actual
        // transform also negates X (the 180° Z rotation flips both).
        s.quat[0] = qx;
        s.quat[1] = qy;
        s.quat[2] = qz;
        s.quat[3] = qw;
        // Colors decode through (packed/255 - 0.5) / 0.15 → sh0,
        // then RGB = sh0 * 0.282095 + 0.5 (clamped to [0,1]). Pre-
        // computing the linear coefficients keeps it cheap:
        //   final = (packed - 127.5) * (0.282095 / (255 * 0.15)) + 0.5
        const float k = 0.282095f / (255.0f * 0.15f);
        auto dec = [&](uint8_t p) -> float {
            float v = (static_cast<float>(p) - 127.5f) * k + 0.5f;
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            return v;
        };
        // Spark's `floatToUint8` uses Math.round (utils.ts:163);
        // truncation here would put us 1 LSB low on every channel.
        auto enc = [](float f) -> uint8_t {
            float r = std::round(255.0f * f);
            if (r <   0.0f) r =   0.0f;
            if (r > 255.0f) r = 255.0f;
            return static_cast<uint8_t>(r);
        };
        s.rgba[0] = enc(dec(rgb_blk[std::size_t(i)*3 + 0]));
        s.rgba[1] = enc(dec(rgb_blk[std::size_t(i)*3 + 1]));
        s.rgba[2] = enc(dec(rgb_blk[std::size_t(i)*3 + 2]));
        s.rgba[3] = alpha_blk[std::size_t(i)];
        out.push_back(s);
    }
    // Bbox + radius stats so the renderer can calibrate the
    // splat-cloud world transform without me guessing.
    if (!out.empty()) {
        float mn[3] = { out[0].pos[0], out[0].pos[1], out[0].pos[2] };
        float mx[3] = { out[0].pos[0], out[0].pos[1], out[0].pos[2] };
        float r_max = out[0].scales[0], r_min = r_max;
        for (const auto& s : out) {
            for (int k = 0; k < 3; ++k) {
                if (s.pos[k] < mn[k]) mn[k] = s.pos[k];
                if (s.pos[k] > mx[k]) mx[k] = s.pos[k];
                if (s.scales[k] < r_min) r_min = s.scales[k];
                if (s.scales[k] > r_max) r_max = s.scales[k];
            }
        }
        std::fprintf(stderr,
            "[splat] bbox X[%.3f, %.3f] Y[%.3f, %.3f] Z[%.3f, %.3f] "
            "scale[%.4f, %.4f]\n",
            mn[0], mx[0], mn[1], mx[1], mn[2], mx[2], r_min, r_max);
    }
    std::fprintf(stderr,
        "[splat] decoded %zu splats from %s (v%u, sh%u, frac_bits=%u)\n",
        out.size(), path.c_str(), version, sh_degree, frac_bits);
    return out;
}
