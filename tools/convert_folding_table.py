#!/usr/bin/env python3
"""Convert the Sketchfab folding-table GLB into the datacenter table.

Replaces convert_dc_table.py's Hunyuan workbench (bake artifacts, and the
±7 square force-fit stretched it 2.5x in Z). The folding table keeps its
real proportions instead:

  Y (height) scaled exactly so the feet land on the room floor
             (Y = -8.268) with the top at Y = 0 — the TABLE_TOP_Y
             contract the board / captures / clock all rely on.
  X (length) uniform with Y -> a realistic long 6-ft table (~±10).
  Z (depth)  the one compromise: stretched to ±5.3 (vs ±4.05 uniform,
             a 1.31x stretch) so the retro board slab (±4.55) and the
             datacenter capture rows (pulled in to Z0 ±4.65 in
             board_renderer.cpp) stay on the tabletop.

Run INSIDE Blender:

    blender -b --factory-startup -noaudio \
        --python tools/convert_folding_table.py -- \
        --glb ~/retro_chess/folding_table.glb --out-dir models/table

Textures: the GLB's glTF PBR set is split into the engine's three maps
(diffuse / roughness=G of metallicRoughness / metalness=B), 2048 JPEG.
UVs as authored (engine clock-texture path samples (u, 1-v), matching
Blender's V-up import convention).
"""
import argparse
import struct
import sys

import bpy
import numpy as np

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ap = argparse.ArgumentParser()
ap.add_argument("--glb", required=True)
ap.add_argument("--out-dir", default="models/table")
ap.add_argument("--depth-half", type=float, default=5.3,
                help="fitted Z half-extent (engine units)")
ap.add_argument("--floor-y", type=float, default=-8.268,
                help="room floor plane (table top sits at 0)")
args = ap.parse_args(argv)

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=args.glb)
dg = bpy.context.evaluated_depsgraph_get()

P, N, UV = [], [], []
for ob in bpy.data.objects:
    if ob.type != "MESH":
        continue
    ev = ob.evaluated_get(dg)
    me = ev.to_mesh()
    me.calc_loop_triangles()
    if hasattr(me, "calc_normals_split"):
        me.calc_normals_split()
    mw = np.array(ev.matrix_world)
    nmw = np.linalg.inv(mw[:3, :3]).T
    uvl = me.uv_layers.active.data
    for tri in me.loop_triangles:
        for li in tri.loops:
            lo = me.loops[li]
            P.append(mw[:3, :3] @ np.array(me.vertices[lo.vertex_index].co) + mw[:3, 3])
            N.append(nmw @ np.array(lo.normal))
            UV.append(list(uvl[li].uv))
    ev.to_mesh_clear()
P, N, UV = np.array(P), np.array(N), np.array(UV)
print(f"imported {len(P) // 3} tris, blender bbox {P.min(0).round(3)} .. {P.max(0).round(3)}")

# Blender Z-up -> engine Y-up (+Z front): (x, y, z) = (bx, bz, -by)
Pe = np.stack([P[:, 0], P[:, 2], -P[:, 1]], axis=1)
Ne = np.stack([N[:, 0], N[:, 2], -N[:, 1]], axis=1)

mn, mx = Pe.min(0), Pe.max(0)
sy = -args.floor_y / (mx[1] - mn[1])          # feet on floor, top at 0
sx = sy                                        # length keeps true proportion
sz = args.depth_half / ((mx[2] - mn[2]) / 2)   # the one stretch
scale = np.array([sx, sy, sz])
centre = np.array([(mn[0] + mx[0]) / 2, mx[1], (mn[2] + mx[2]) / 2])
Pf = (Pe - centre) * scale
Nf = Ne / scale
Nf /= (np.linalg.norm(Nf, axis=1, keepdims=True) + 1e-12)
print(f"fitted bbox {Pf.min(0).round(3)} .. {Pf.max(0).round(3)}  "
      f"scale {scale.round(3)} (Z stretch {sz / sy:.2f}x)")

flat = np.concatenate([Pf, Nf, UV], axis=1).astype(np.float32)
path = f"{args.out_dir}/dc_table.uvmesh"
with open(path, "wb") as fh:
    fh.write(b"UVME")
    fh.write(struct.pack("<I", len(flat)))
    fh.write(flat.tobytes())
print(f"wrote {path}: {len(flat) // 3} tris")

# Textures: walk the material's node graph for the glTF PBR images.
from PIL import Image

mat = bpy.data.materials[0]
bsdf = next(n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED")


def image_for(socket_name):
    sock = bsdf.inputs[socket_name]
    seen, stack = set(), [lk.from_node for lk in sock.links]
    while stack:
        n = stack.pop()
        if n.name in seen:
            continue
        seen.add(n.name)
        if n.type == "TEX_IMAGE":
            return n.image
        stack.extend(lk.from_node for inp in n.inputs for lk in inp.links)
    return None


def as_np(img):
    w, h = img.size
    a = np.array(img.pixels[:], dtype=np.float32).reshape(h, w, img.channels)
    return (a[::-1, :, :3].clip(0, 1) * 255).astype(np.uint8)  # flip: GL rows


base = image_for("Base Color")
mr = image_for("Roughness")  # glTF metallicRoughness feeds both via Separate
print("base image:", base.name if base else None,
      " metallicRoughness image:", mr.name if mr else None)
Image.fromarray(as_np(base)).resize((2048, 2048), Image.LANCZOS) \
    .save(f"{args.out_dir}/dc_table_diffuse.jpg", quality=92)
mr_np = as_np(mr)
Image.fromarray(mr_np[..., 1]).resize((2048, 2048), Image.LANCZOS) \
    .save(f"{args.out_dir}/dc_table_roughness.jpg", quality=90)
Image.fromarray(mr_np[..., 2]).resize((2048, 2048), Image.LANCZOS) \
    .save(f"{args.out_dir}/dc_table_metalness.jpg", quality=90)
print("textures written (diffuse + roughness[G] + metalness[B])")
