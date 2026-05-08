"""Same scale/rotate/centre transform as convert_clock.py, but
exports the body sub-object as a custom binary `.uvmesh` file
that includes UV coordinates per vertex (the standard STL format
we use elsewhere strips them). Only the body needs this — the
needles/glass keep their plain STL form because they don't sample
any texture.

uvmesh layout (little-endian):
  bytes  0..3    : magic "UVME"
  bytes  4..7    : uint32 vertex_count
  bytes  8..    : per-vertex (pos.xyz float, normal.xyz float, uv.xy float)
                   = 8 floats = 32 bytes
"""

import bpy
import math
import os
import struct
import sys
import mathutils

OBJ = "/home/jaherrero/chess_clock/extracted/ChessClock_Sketchfab/ChessClock.obj"
OUT_DIR = "/home/jaherrero/claude_workspace/3d_chess/models/clock"

bpy.ops.wm.read_factory_settings(use_empty=True)
try:
    bpy.ops.wm.obj_import(filepath=OBJ)
except AttributeError:
    bpy.ops.import_scene.obj(filepath=OBJ)

SUB_OBJECTS = {
    "Clock.001":      "clock_body",
    "Cursore.dx.001": "clock_dial_r",
    "Cursore.sx.001": "clock_dial_l",
}
sub_objs = {bname: bpy.data.objects.get(name)
            for name, bname in SUB_OBJECTS.items()}
missing = [k for k, v in sub_objs.items() if v is None]
if missing:
    print(f"Sub-objects missing: {missing}"); sys.exit(1)
body = sub_objs["clock_body"]

# Apply object transform.
bpy.ops.object.select_all(action="DESELECT")
body.select_set(True)
bpy.context.view_layer.objects.active = body
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

# Compute global bbox over all sub-objects so the scale matches
# what convert_clock.py does for the STLs (otherwise the body
# would float in a different coord space than the needles/glass).
all_meshes = [o for o in bpy.data.objects if o.type == "MESH"]
for m in all_meshes:
    if m is body: continue
    bpy.ops.object.select_all(action="DESELECT")
    m.select_set(True)
    bpy.context.view_layer.objects.active = m
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

xs, ys, zs = [], [], []
for m in all_meshes:
    for v in m.data.vertices:
        xs.append(v.co.x); ys.append(v.co.y); zs.append(v.co.z)
bb_min = (min(xs), min(ys), min(zs))
bb_max = (max(xs), max(ys), max(zs))
size = (bb_max[0]-bb_min[0], bb_max[1]-bb_min[1], bb_max[2]-bb_min[2])
target = 3.0
scale = target / max(size)

mat_scale  = mathutils.Matrix.Scale(scale, 4)
mat_rotate = mathutils.Matrix.Rotation(math.radians(-90.0), 4, "X")
m_sr = mat_rotate @ mat_scale

# Bbox after scale+rotate (using all sub-objects to match the
# centring translation convert_clock.py applies).
def transform_v(v, m):
    p = m @ mathutils.Vector((v.x, v.y, v.z, 1.0))
    return (p.x, p.y, p.z)
xs2, ys2, zs2 = [], [], []
for m in all_meshes:
    for v in m.data.vertices:
        x, y, z = transform_v(v.co, m_sr)
        xs2.append(x); ys2.append(y); zs2.append(z)
bb2_min = (min(xs2), min(ys2), min(zs2))
bb2_max = (max(xs2), max(ys2), max(zs2))
tx = -0.5 * (bb2_min[0] + bb2_max[0])
ty = -bb2_min[1]
tz = -0.5 * (bb2_min[2] + bb2_max[2])
combined = mathutils.Matrix.Translation((tx, ty, tz)) @ m_sr

for m in sub_objs.values():
    m.data.transform(combined)

# Compute smooth normals so the writer below uses up-to-date data.
body.data.calc_loop_triangles()
# Modern Blender uses corner_normal calculation via mesh settings.
try:
    body.data.calc_normals_split()
except Exception:
    pass

uv_layer = body.data.uv_layers.active.data if body.data.uv_layers.active else None
if uv_layer is None:
    print("No UV layer on body"); sys.exit(1)

# Walk loop_triangles, emitting per-vertex (pos, normal, uv).
def emit_uvmesh(out_path, decimate_ratio=None):
    if decimate_ratio is not None and decimate_ratio < 1.0:
        # Make a working copy so the modifier doesn't mutate `body`.
        bpy.ops.object.select_all(action="DESELECT")
        body.select_set(True)
        bpy.context.view_layer.objects.active = body
        bpy.ops.object.duplicate(linked=False)
        copy = bpy.context.view_layer.objects.active
        n = len(copy.data.polygons)
        ratio = max(0.05, decimate_ratio)
        mod = copy.modifiers.new(name="Decimate", type="DECIMATE")
        mod.ratio = ratio
        mod.use_collapse_triangulate = True
        bpy.ops.object.modifier_apply(modifier=mod.name)
        target_obj = copy
    else:
        target_obj = body

    me = target_obj.data
    me.calc_loop_triangles()
    try:
        me.calc_normals_split()
    except Exception:
        pass
    uv_data = me.uv_layers.active.data if me.uv_layers.active else None

    pieces = []
    for tri in me.loop_triangles:
        for i in range(3):
            li = tri.loops[i]              # loop index → uv lookup
            vi = me.loops[li].vertex_index
            v = me.vertices[vi]
            n = tri.split_normals[i]
            u, w = (uv_data[li].uv if uv_data else (0.0, 0.0))
            pieces.append((v.co.x, v.co.y, v.co.z,
                           n[0], n[1], n[2],
                           u, w))
    print(f"  {out_path}: {len(pieces)} vertices ({len(pieces)//3} tris)")

    with open(out_path, "wb") as f:
        f.write(b"UVME")
        f.write(struct.pack("<I", len(pieces)))
        for p in pieces:
            f.write(struct.pack("<ffffffff", *p))

    if target_obj is not body:
        bpy.data.objects.remove(target_obj, do_unlink=True)

# Export each sub-object as its own uvmesh.
for bname, obj in sub_objs.items():
    target = obj
    # Use `body` (the main body) decimation only for the heavy
    # body mesh; the dials are tiny (a few hundred tris).
    full_path = os.path.join(OUT_DIR, f"{bname}.uvmesh")
    web_path  = os.path.join(OUT_DIR, f"{bname}_web.uvmesh")
    # Temporarily swap `body` reference for emit_uvmesh's decimate
    # path which reads it.
    saved_body = body
    globals()["body"] = obj
    if bname == "clock_body":
        emit_uvmesh(full_path)
        emit_uvmesh(web_path,
                    decimate_ratio=20000.0 /
                                   max(len(obj.data.polygons), 1))
    else:
        emit_uvmesh(full_path)
        emit_uvmesh(web_path)         # tiny enough to ship full
    globals()["body"] = saved_body
print("done")
