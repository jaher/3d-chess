"""
Pack STL chess piece models into a compact indexed-mesh format for the
WebAssembly build, where bandwidth dominates load time.

Binary STL stores each triangle as three fully-duplicated vertices plus
a face normal — with an average vertex sharing count of ~6, that's
about 3x redundancy on positions. The face normal is also wasted: the
runtime recomputes smooth per-vertex normals from geometry regardless.

This tool reads `models-web/*.stl` and writes `models-web-packed/*.stl`
containing gzipped IMSH data:

    magic[4]         = "IMSH"
    version: u32     = 1
    vertex_count: u32
    triangle_count: u32
    positions:       vertex_count   x float32[3]
    indices:         triangle_count x uint32[3]

The files keep the `.stl` extension so the C++ loader can content-sniff
and the web Makefile doesn't need per-file rules. For 80k-triangle
decimated pieces this drops each file from ~4 MB to ~1 MB before gzip
and ~0.5 MB after — a 6-8x reduction on the web bundle.

Run:
    python3 tools/pack_meshes.py
"""

import argparse
import gzip
import os
import struct
import sys


REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DEFAULT_SRC = os.path.join(REPO_ROOT, "models-web")
DEFAULT_DST = os.path.join(REPO_ROOT, "models-web-packed")

PIECES = ["King.stl", "Queen.stl", "Bishop.stl", "Knight.stl", "Rook.stl", "Pawn.stl"]


def read_stl_binary(path: str):
    """Yield (v0, v1, v2) tuples for each triangle in a binary STL file."""
    with open(path, "rb") as f:
        f.read(80)  # header
        (tri_count,) = struct.unpack("<I", f.read(4))
        for _ in range(tri_count):
            data = f.read(50)
            # data[0:12] = face normal (ignored), data[48:50] = attr bytes
            v0 = struct.unpack("<fff", data[12:24])
            v1 = struct.unpack("<fff", data[24:36])
            v2 = struct.unpack("<fff", data[36:48])
            yield v0, v1, v2


def build_index(triangles):
    """Collapse byte-identical vertex tuples into a vertex list + index list."""
    vmap: dict[tuple, int] = {}
    verts: list[tuple] = []
    indices: list[int] = []
    for tri in triangles:
        for v in tri:
            idx = vmap.get(v)
            if idx is None:
                idx = len(verts)
                vmap[v] = idx
                verts.append(v)
            indices.append(idx)
    return verts, indices


def encode_imsh(verts, indices) -> bytes:
    tri_count = len(indices) // 3
    parts = [struct.pack("<4sIII", b"IMSH", 1, len(verts), tri_count)]
    # Flattened position payload.
    parts.append(struct.pack(f"<{len(verts) * 3}f",
                             *(c for v in verts for c in v)))
    # Flattened index payload.
    parts.append(struct.pack(f"<{len(indices)}I", *indices))
    return b"".join(parts)


def pack_one(src_path: str, dst_path: str) -> None:
    tris = list(read_stl_binary(src_path))
    verts, indices = build_index(tris)
    raw = encode_imsh(verts, indices)
    with gzip.open(dst_path, "wb", compresslevel=9) as f:
        f.write(raw)

    src_sz = os.path.getsize(src_path)
    dst_sz = os.path.getsize(dst_path)
    ratio = dst_sz / src_sz if src_sz else 0.0
    print(f"[pack] {os.path.basename(src_path)}: "
          f"{len(tris)} tris, {len(verts)} unique verts  "
          f"{src_sz/1024:.0f} KB -> {dst_sz/1024:.0f} KB  ({ratio:.1%})")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--src", default=DEFAULT_SRC,
                    help=f"STL source directory (default: {DEFAULT_SRC})")
    ap.add_argument("--dst", default=DEFAULT_DST,
                    help=f"packed output directory (default: {DEFAULT_DST})")
    args = ap.parse_args()

    os.makedirs(args.dst, exist_ok=True)

    total_src = 0
    total_dst = 0
    for name in PIECES:
        src = os.path.join(args.src, name)
        dst = os.path.join(args.dst, name)
        if not os.path.exists(src):
            print(f"error: missing {src}", file=sys.stderr)
            return 1
        pack_one(src, dst)
        total_src += os.path.getsize(src)
        total_dst += os.path.getsize(dst)

    print(f"[pack] total: {total_src/1024:.0f} KB -> {total_dst/1024:.0f} KB "
          f"({total_dst/total_src:.1%})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
