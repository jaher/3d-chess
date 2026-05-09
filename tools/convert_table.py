"""Convert the Sketchfab "square table" DAE into a single .uvmesh
that the chess game can render under the chessboard, plus copy the
PBR JPEG textures into models/table/.

uvmesh layout matches convert_clock.py — see that file for the
field definition (8 floats / vertex: pos.xyz, normal.xyz, uv.xy).

The table DAE is small and self-contained (single mesh, parallel
position/normal/texcoord arrays indexed by a single vcount=3
polylist), so we parse the XML directly instead of going through
Blender — Blender 4 dropped the bpy.ops.wm.collada_import op and
re-introducing the addon is more pain than it's worth here.

Run with: python3 tools/convert_table.py
"""

import math
import os
import shutil
import struct
import sys
import xml.etree.ElementTree as ET

DAE     = os.path.expanduser("~/chess_table/model.dae")
TEX_SRC = os.path.expanduser("~/chess_table/model_textures")
OUT_DIR = "/home/jaherrero/claude_workspace/3d_chess/models/table"

# Chessboard width in world units is about 9 (8 squares + frame). Make
# the table large enough that the board reads as comfortably sat on
# top without spilling past the edge — 14 world units across.
TARGET_WIDTH = 14.0

NS = {"c": "http://www.collada.org/2005/11/COLLADASchema"}

os.makedirs(OUT_DIR, exist_ok=True)

tree = ET.parse(DAE)
root = tree.getroot()

# --- Parse the single mesh's source arrays (positions, normals, uvs).
mesh = root.find(".//c:library_geometries/c:geometry/c:mesh", NS)
if mesh is None:
    print("no <mesh> in DAE"); sys.exit(1)

# Each <source> has a unique id and a <float_array> child. Index by
# stripping the leading '#' the input semantics use.
sources = {}
for src in mesh.findall("c:source", NS):
    sid = src.attrib["id"]
    fa = src.find("c:float_array", NS)
    if fa is None:
        continue
    nums = [float(x) for x in fa.text.split()]
    sources[sid] = nums

# <vertices> remaps a vertex index into per-stream arrays. Resolve
# semantics → array.
verts_node = mesh.find("c:vertices", NS)
verts_inputs = {}
for inp in verts_node.findall("c:input", NS):
    sem = inp.attrib["semantic"]
    src_id = inp.attrib["source"].lstrip("#")
    verts_inputs[sem] = src_id

# Get the polylist's input streams. The asset uses a single
# offset=0 VERTEX input, so face indices are 1 per vertex.
polylist = mesh.find("c:polylist", NS)
if polylist is None:
    print("no <polylist>"); sys.exit(1)
inputs = polylist.findall("c:input", NS)
strides_per_index = max(int(inp.attrib["offset"]) for inp in inputs) + 1
vertex_input = next(inp for inp in inputs if inp.attrib["semantic"] == "VERTEX")
vertex_offset = int(vertex_input.attrib["offset"])
vcounts = [int(x) for x in polylist.find("c:vcount", NS).text.split()]
flat_indices = [int(x) for x in polylist.find("c:p", NS).text.split()]

pos_arr = sources[verts_inputs["POSITION"]]
nrm_arr = sources.get(verts_inputs.get("NORMAL", ""), None)
uv_arr  = sources.get(verts_inputs.get("TEXCOORD", ""), None)

# Walk faces. Each face is a fan of `vcount` vertices; we triangulate
# using the first vertex (the asset already ships triangles with
# vcount=3 everywhere, so this is mostly defensive).
N = len(pos_arr) // 3
def vert_index(i):
    return flat_indices[i * strides_per_index + vertex_offset]

raw_verts = []  # list of (px, py, pz, nx, ny, nz, u, v)
cursor = 0
for vc in vcounts:
    face_idx = [vert_index(cursor + k) for k in range(vc)]
    cursor += vc
    # Triangulate fan.
    for k in range(1, vc - 1):
        for vi in (face_idx[0], face_idx[k], face_idx[k + 1]):
            px, py, pz = pos_arr[vi*3:vi*3+3]
            if nrm_arr is not None and vi*3+2 < len(nrm_arr):
                nx, ny, nz = nrm_arr[vi*3:vi*3+3]
            else:
                nx, ny, nz = 0.0, 1.0, 0.0
            if uv_arr is not None and vi*2+1 < len(uv_arr):
                u, v = uv_arr[vi*2:vi*2+2]
            else:
                u, v = 0.0, 0.0
            raw_verts.append((px, py, pz, nx, ny, nz, u, v))
print(f"DAE: {N} unique verts, {len(raw_verts)//3} triangles")

# --- Compute bbox so we can scale + center.
xs = [v[0] for v in raw_verts]
ys = [v[1] for v in raw_verts]
zs = [v[2] for v in raw_verts]
bb_min = (min(xs), min(ys), min(zs))
bb_max = (max(xs), max(ys), max(zs))
size = (bb_max[0]-bb_min[0], bb_max[1]-bb_min[1], bb_max[2]-bb_min[2])
print(f"raw bbox X{bb_min[0]:.2f}..{bb_max[0]:.2f} "
      f"Y{bb_min[1]:.2f}..{bb_max[1]:.2f} "
      f"Z{bb_min[2]:.2f}..{bb_max[2]:.2f}  size={size}")

# Smallest extent is the "height" axis (table is wider than tall).
sorted_axes = sorted([(size[0], 'X'), (size[1], 'Y'), (size[2], 'Z')])
height_axis = sorted_axes[0][1]
horiz_max = max(sorted_axes[1][0], sorted_axes[2][0])
scale = TARGET_WIDTH / horiz_max
print(f"horiz_max={horiz_max:.2f}, height_axis={height_axis}, scale={scale:.3f}")

# Build a 3×3 rotation that puts +Y up (chess game convention) given
# whichever axis the DAE encodes as up.
def apply(p, rot):
    px, py, pz = p
    return (rot[0][0]*px + rot[0][1]*py + rot[0][2]*pz,
            rot[1][0]*px + rot[1][1]*py + rot[1][2]*pz,
            rot[2][0]*px + rot[2][1]*py + rot[2][2]*pz)

if height_axis == 'Z':
    # Z-up → Y-up: rotate -90° around X.
    rot = [[1, 0, 0],
           [0, 0, 1],
           [0, -1, 0]]
elif height_axis == 'X':
    # X-up → Y-up: rotate +90° around Z (so +X → +Y).
    rot = [[0, -1, 0],
           [1, 0, 0],
           [0, 0, 1]]
else:
    rot = [[1, 0, 0],
           [0, 1, 0],
           [0, 0, 1]]

# Apply scale + rotation to every vertex; recompute bbox so we can
# centre horizontally and put the top of the table at Y=0.
xformed = []
for (px, py, pz, nx, ny, nz, u, v) in raw_verts:
    sp = (px * scale, py * scale, pz * scale)
    sp = apply(sp, rot)
    sn = apply((nx, ny, nz), rot)
    xformed.append((sp[0], sp[1], sp[2], sn[0], sn[1], sn[2], u, v))

ys2 = [v[1] for v in xformed]
xs2 = [v[0] for v in xformed]
zs2 = [v[2] for v in xformed]
bb2_min = (min(xs2), min(ys2), min(zs2))
bb2_max = (max(xs2), max(ys2), max(zs2))
print(f"post-scale/rot bbox X{bb2_min[0]:.2f}..{bb2_max[0]:.2f} "
      f"Y{bb2_min[1]:.2f}..{bb2_max[1]:.2f} "
      f"Z{bb2_min[2]:.2f}..{bb2_max[2]:.2f}")
tx = -0.5 * (bb2_min[0] + bb2_max[0])
ty = -bb2_max[1]   # top surface at Y=0
tz = -0.5 * (bb2_min[2] + bb2_max[2])

final = []
for (px, py, pz, nx, ny, nz, u, v) in xformed:
    final.append((px + tx, py + ty, pz + tz, nx, ny, nz, u, v))

# Write the .uvmesh.
out_path = os.path.join(OUT_DIR, "table.uvmesh")
with open(out_path, "wb") as f:
    f.write(b"UVME")
    f.write(struct.pack("<I", len(final)))
    for p in final:
        f.write(struct.pack("<ffffffff", *p))
print(f"wrote {out_path} ({len(final)} verts, {len(final)//3} tris)")

# Copy textures.
TEX_MAP = {
    "DefaultMaterial_albedo.jpg":    "table_albedo.jpg",
    "DefaultMaterial_normal.jpg":    "table_normal.jpg",
    "DefaultMaterial_roughness.jpg": "table_roughness.jpg",
    "DefaultMaterial_metallic.jpg":  "table_metallic.jpg",
    "DefaultMaterial_AO.jpg":        "table_ao.jpg",
}
for src_name, dst_name in TEX_MAP.items():
    src = os.path.join(TEX_SRC, src_name)
    dst = os.path.join(OUT_DIR, dst_name)
    if os.path.isfile(src):
        shutil.copyfile(src, dst)
        print(f"copied {src} -> {dst}")
    else:
        print(f"missing {src}")
print("done")
