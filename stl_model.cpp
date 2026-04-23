#include "stl_model.h"

#include "linalg.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>

Vertex BoundingBox::center() const {
    return {(min.x + max.x) / 2.0f,
            (min.y + max.y) / 2.0f,
            (min.z + max.z) / 2.0f};
}

float BoundingBox::max_extent() const {
    float dx = max.x - min.x;
    float dy = max.y - min.y;
    float dz = max.z - min.z;
    float m = dx;
    if (dy > m) m = dy;
    if (dz > m) m = dz;
    return m;
}

void StlModel::load(const std::string& path) {
    triangles_.clear();

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        throw std::runtime_error("Cannot open file: " + path);

    // Read 80-byte header
    char header[80];
    file.read(header, 80);
    if (!file)
        throw std::runtime_error("Failed to read STL header");

    // Check if it looks like ASCII STL
    if (std::string(header, 5) == "solid") {
        // Could be ASCII, or binary with "solid" in header.
        // Disambiguate by checking whether the size matches the binary format.
        uint32_t num_triangles = 0;
        file.read(reinterpret_cast<char*>(&num_triangles), 4);

        file.seekg(0, std::ios::end);
        auto file_size = file.tellg();

        if (file_size == static_cast<std::streampos>(80 + 4 + 50 * num_triangles)) {
            file.seekg(84);
            load_binary(file, num_triangles);
        } else {
            file.seekg(0);
            load_ascii(file);
        }
    } else {
        uint32_t num_triangles = 0;
        file.read(reinterpret_cast<char*>(&num_triangles), 4);
        load_binary(file, num_triangles);
    }

    compute_bounding_box();
}

namespace {

// Interior angle of a triangle at vertex `apex`, between edges to `b` and `c`.
static float corner_angle(const Vec3& apex, const Vec3& b, const Vec3& c) {
    Vec3 e1 = normalize(b - apex);
    Vec3 e2 = normalize(c - apex);
    float d = dot(e1, e2);
    if (d > 1.0f) d = 1.0f;
    if (d < -1.0f) d = -1.0f;
    return std::acos(d);
}

// Hash key for vertex positions (quantized to dedupe near-duplicates).
struct PosKey {
    int x, y, z;
    bool operator==(const PosKey& o) const {
        return x == o.x && y == o.y && z == o.z;
    }
};

struct PosKeyHash {
    size_t operator()(const PosKey& k) const {
        size_t h = static_cast<size_t>(k.x) * 73856093u;
        h ^= static_cast<size_t>(k.y) * 19349663u;
        h ^= static_cast<size_t>(k.z) * 83492791u;
        return h;
    }
};

static PosKey quantize(const Vertex& v, float step) {
    return { static_cast<int>(std::lround(v.x / step)),
             static_cast<int>(std::lround(v.y / step)),
             static_cast<int>(std::lround(v.z / step)) };
}

} // namespace

std::vector<float> StlModel::build_vertex_buffer(float crease_angle_deg) const {
    Vertex c = bbox_.center();
    float scale = 2.0f / bbox_.max_extent();

    const size_t T = triangles_.size();

    // Flat-shaded fast path
    if (crease_angle_deg <= 0.0f) {
        std::vector<float> buf;
        buf.reserve(T * 6 * 3);
        for (const auto& tri : triangles_) {
            const Vertex* verts[3] = {&tri.v0, &tri.v1, &tri.v2};
            for (int i = 0; i < 3; i++) {
                buf.push_back(tri.normal.x);
                buf.push_back(tri.normal.y);
                buf.push_back(tri.normal.z);
                buf.push_back((verts[i]->x - c.x) * scale);
                buf.push_back((verts[i]->y - c.y) * scale);
                buf.push_back((verts[i]->z - c.z) * scale);
            }
        }
        return buf;
    }

    // Quantize positions at 1e-4 of the model's max extent so neighbouring
    // triangles that share a vertex line up even with float drift.
    float quant_step = bbox_.max_extent() * 1e-4f;
    if (quant_step <= 0.0f) quant_step = 1e-5f;

    // Per-corner precomputed data: face normal, angle weight at that corner.
    std::vector<Vec3> face_n(T);
    std::vector<float> corner_w(T * 3);
    for (size_t t = 0; t < T; t++) {
        const Triangle& tri = triangles_[t];
        // Prefer a geometric normal (some STLs have zero/garbage normals);
        // fall back to the stored normal if the triangle is degenerate.
        Vec3 v0{tri.v0.x, tri.v0.y, tri.v0.z};
        Vec3 v1{tri.v1.x, tri.v1.y, tri.v1.z};
        Vec3 v2{tri.v2.x, tri.v2.y, tri.v2.z};
        Vec3 e1 = v1 - v0, e2 = v2 - v0;
        Vec3 geo{ e1.y*e2.z - e1.z*e2.y,
                  e1.z*e2.x - e1.x*e2.z,
                  e1.x*e2.y - e1.y*e2.x };
        if (length(geo) > 1e-20f) face_n[t] = normalize(geo);
        else                      face_n[t] = normalize({tri.normal.x, tri.normal.y, tri.normal.z});

        corner_w[t*3 + 0] = corner_angle(v0, v1, v2);
        corner_w[t*3 + 1] = corner_angle(v1, v2, v0);
        corner_w[t*3 + 2] = corner_angle(v2, v0, v1);
    }

    // Group corners (triangle, which-vertex) by quantized position.
    std::unordered_map<PosKey, std::vector<std::pair<uint32_t, uint8_t>>, PosKeyHash> groups;
    groups.reserve(T * 3);
    for (size_t t = 0; t < T; t++) {
        const Vertex* verts[3] = {&triangles_[t].v0, &triangles_[t].v1, &triangles_[t].v2};
        for (uint8_t i = 0; i < 3; i++) {
            PosKey k = quantize(*verts[i], quant_step);
            groups[k].push_back({static_cast<uint32_t>(t), i});
        }
    }

    float crease_cos = std::cos(crease_angle_deg * static_cast<float>(M_PI) / 180.0f);

    std::vector<float> buf;
    buf.reserve(T * 6 * 3);

    for (size_t t = 0; t < T; t++) {
        const Triangle& tri = triangles_[t];
        const Vertex* verts[3] = {&tri.v0, &tri.v1, &tri.v2};
        const Vec3& fn = face_n[t];

        for (uint8_t i = 0; i < 3; i++) {
            PosKey k = quantize(*verts[i], quant_step);
            const auto& group = groups[k];

            Vec3 sum{0, 0, 0};
            for (const auto& [ot, oi] : group) {
                const Vec3& ofn = face_n[ot];
                // Crease threshold: only merge faces whose normal is within
                // crease_angle of this corner's face normal.
                if (dot(fn, ofn) < crease_cos) continue;
                float w = corner_w[ot*3 + oi];
                sum = sum + ofn * w;
            }

            Vec3 n = normalize(sum);
            if (length(sum) < 1e-20f) n = fn; // degenerate fallback

            buf.push_back(n.x);
            buf.push_back(n.y);
            buf.push_back(n.z);
            buf.push_back((verts[i]->x - c.x) * scale);
            buf.push_back((verts[i]->y - c.y) * scale);
            buf.push_back((verts[i]->z - c.z) * scale);
        }
    }
    return buf;
}

void StlModel::load_binary(std::ifstream& file, uint32_t count) {
    triangles_.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        file.read(reinterpret_cast<char*>(&triangles_[i]), 48);
        uint16_t attr;
        file.read(reinterpret_cast<char*>(&attr), 2);
        if (!file)
            throw std::runtime_error("Unexpected end of binary STL data");
    }
}

void StlModel::load_ascii(std::ifstream& file) {
    std::string line;
    Triangle tri{};

    while (std::getline(file, line)) {
        // Trim leading whitespace
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        if (line.rfind("facet normal", 0) == 0) {
            std::sscanf(line.c_str(), "facet normal %f %f %f",
                        &tri.normal.x, &tri.normal.y, &tri.normal.z);
        } else if (line.rfind("vertex", 0) == 0) {
            Vertex v;
            std::sscanf(line.c_str(), "vertex %f %f %f", &v.x, &v.y, &v.z);
            if (tri.v0.x == 0 && tri.v0.y == 0 && tri.v0.z == 0 &&
                tri.v1.x == 0 && tri.v1.y == 0 && tri.v1.z == 0 &&
                tri.v2.x == 0 && tri.v2.y == 0 && tri.v2.z == 0) {
                tri.v0 = v;
            } else if (tri.v1.x == 0 && tri.v1.y == 0 && tri.v1.z == 0 &&
                       tri.v2.x == 0 && tri.v2.y == 0 && tri.v2.z == 0) {
                tri.v1 = v;
            } else {
                tri.v2 = v;
            }
        } else if (line.rfind("endfacet", 0) == 0) {
            triangles_.push_back(tri);
            tri = Triangle{};
        }
    }
}

void StlModel::compute_bounding_box() {
    constexpr float inf = std::numeric_limits<float>::max();
    bbox_.min = {inf, inf, inf};
    bbox_.max = {-inf, -inf, -inf};

    auto update = [&](const Vertex& v) {
        if (v.x < bbox_.min.x) bbox_.min.x = v.x;
        if (v.y < bbox_.min.y) bbox_.min.y = v.y;
        if (v.z < bbox_.min.z) bbox_.min.z = v.z;
        if (v.x > bbox_.max.x) bbox_.max.x = v.x;
        if (v.y > bbox_.max.y) bbox_.max.y = v.y;
        if (v.z > bbox_.max.z) bbox_.max.z = v.z;
    };

    for (const auto& tri : triangles_) {
        update(tri.v0);
        update(tri.v1);
        update(tri.v2);
    }
}
