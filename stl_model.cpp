#include "stl_model.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

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

std::vector<float> StlModel::build_vertex_buffer() const {
    Vertex c = bbox_.center();
    float scale = 2.0f / bbox_.max_extent();

    std::vector<float> buf;
    buf.reserve(triangles_.size() * 6 * 3); // 3 verts * 6 floats each

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
