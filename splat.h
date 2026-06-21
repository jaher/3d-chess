#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Niantic SPZ (legacy v1-3, gzip-framed). One Splat is a 3D Gaussian:
// position + per-axis scales (log) + rotation (quaternion) + RGB +
// alpha. Spherical-harmonic view-dependent colour is dropped.
// The renderer projects (R · S · Sᵀ · Rᵀ) through a Jacobian to a
// 2D screen-space ellipse following the EWA splatting math from
// Zwicker 2001 (the same approach Spark / 3DGS use).
struct Splat {
    float pos[3];
    float scales[3];   // already exponentiated, in splat-local units
    double quat[4];    // (x, y, z, w), normalized — kept in double so
                       // the dump path's quat roundtrip matches JS
                       // (Spark uses double throughout) bit-for-bit.
    uint8_t rgba[4];
};

// Read & decode an SPZ file from disk. Returns empty on failure.
std::vector<Splat> splat_load_spz(const std::string& path);
