#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

struct Vertex {
    float x, y, z;
};

struct Triangle {
    Vertex normal;
    Vertex v0, v1, v2;
};

struct BoundingBox {
    Vertex min;
    Vertex max;

    Vertex center() const;
    float max_extent() const;
};

class StlModel {
  public:
    StlModel() = default;

    // Load an STL file (auto-detects ASCII vs binary)
    void load(const std::string& path);

    const std::vector<Triangle>& triangles() const { return triangles_; }
    const BoundingBox& bounding_box() const { return bbox_; }
    size_t triangle_count() const { return triangles_.size(); }

    // Build an interleaved vertex buffer: [nx, ny, nz, x, y, z] per vertex.
    // The model is centered at origin and scaled to fit in a unit sphere.
    // Uses smooth per-vertex normals (angle-weighted average of face
    // normals at each shared vertex position). crease_angle_deg controls
    // sharp-edge preservation: adjacent faces whose dihedral exceeds this
    // threshold aren't merged. Pass a large value (e.g. 180) to smooth
    // everything; pass 0 for flat shading (per-face normals).
    std::vector<float> build_vertex_buffer(float crease_angle_deg = 60.0f) const;

  private:
    std::vector<Triangle> triangles_;
    BoundingBox bbox_;

    void load_binary(std::ifstream& file, uint32_t count);
    void load_ascii(std::ifstream& file);
    void compute_bounding_box();
};
