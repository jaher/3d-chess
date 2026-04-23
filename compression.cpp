#include "compression.h"

#include <zlib.h>

#include <stdexcept>

std::vector<uint8_t> gunzip(const uint8_t* data, std::size_t size) {
    z_stream zs{};
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    zs.avail_in = static_cast<uInt>(size);
    // 16 + MAX_WBITS selects gzip framing (vs raw deflate / zlib).
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK)
        throw std::runtime_error("gunzip: inflateInit2 failed");

    std::vector<uint8_t> out;
    // Typical decompression ratio for our inputs (gzipped IMSH meshes,
    // text, etc.) lands around 6-8x; this reserve avoids reallocs in
    // the common case without over-allocating for tiny inputs.
    out.reserve(size * 8);
    uint8_t chunk[64 * 1024];

    int ret;
    do {
        zs.next_out = chunk;
        zs.avail_out = sizeof(chunk);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret < 0) {
            inflateEnd(&zs);
            throw std::runtime_error("gunzip: inflate failed");
        }
        out.insert(out.end(), chunk, chunk + (sizeof(chunk) - zs.avail_out));
    } while (ret != Z_STREAM_END);

    inflateEnd(&zs);
    return out;
}
