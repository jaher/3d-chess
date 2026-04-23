#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Thin wrappers around zlib that throw std::runtime_error on failure.
// Kept separate from stl_model / audio / etc. so other loaders can
// reuse the same gzip helpers without pulling in mesh headers.

// Inflate a gzip stream (files starting with 0x1f 0x8b) into memory.
std::vector<uint8_t> gunzip(const uint8_t* data, std::size_t size);
