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
// Then five contiguous attribute blocks (in this exact order — the
// Niantic reference's `serializePackedGaussians`):
//   positions  : N × 9 bytes (3 axes, each a 24-bit signed fixed
//                point with `fractional_bits` bits after the binary
//                point)
//   alpha      : N × 1 byte  (uint8)
//   colors     : N × 3 bytes (uint8, encoded as
//                packed = (sh0 * 0.15 + 0.5) * 255 so the inverse is
//                sh0 = (packed/255 - 0.5) / 0.15, and the final RGB
//                is sh0 * 0.282 + 0.5 clamped to [0, 1])
//   scales     : N × 3 bytes (uint8, log-encoded; we take the
//                largest axis as an isotropic radius — see splat.h)
//   rotations  : N × 3 bytes (smallest-three-quaternion compressed
//                into 24 bits; DROPPED, splats treated as isotropic)
//   sh         : N × extra (degree-dependent, skipped for sh=0)
// Total per-splat = 19 bytes, which lines up with the decompressed
// sizes we see from the marble-1.1 model (98,304 splats → 1,867,776
// payload + 16-byte header = 1,867,792).

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
    const std::size_t per_splat = 19 + static_cast<std::size_t>(sh_coeffs);
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
    const uint8_t* rot_blk    = scale_blk  + std::size_t(n) * 3;  // unused
    (void)rot_blk;

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
        // Quaternion: 3 bytes carrying the (x, y, z) components,
        // each unsigned byte 0..255 mapped to (-1, +1) via
        // `q = byte / 127.5 - 1`. Reconstruct w = √(1 - x² - y² - z²).
        // This is Spark's `unpack v2` formula.
        // Stay in double (Splat.quat is double[4]); JS is double
        // throughout, so anything we narrow to float here drifts
        // by 1 ULP.
        const uint8_t* q = rot_blk + std::size_t(i) * 3;
        double qx = static_cast<double>(q[0]) / 127.5 - 1.0;
        double qy = static_cast<double>(q[1]) / 127.5 - 1.0;
        double qz = static_cast<double>(q[2]) / 127.5 - 1.0;
        double qw = std::sqrt(std::max(0.0, 1.0 - qx*qx - qy*qy - qz*qz));
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
