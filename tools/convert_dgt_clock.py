#!/usr/bin/env python3
"""Convert the chessclockdigital Blender asset into the engine's format.

Successor to convert_digital_clock.py (which cleaned up the Hunyuan3D
bake): the source here is a professionally-modelled clock
(~/retro_chess/chessclockdigital_v31_cycles.zip,
chessclockdigital_v3.1_Cycles.blend) with clean 4K PBR atlases, so no
artifact repair is needed. Run INSIDE Blender:

    blender -b --factory-startup -noaudio \
        --python tools/convert_dgt_clock.py -- \
        --blend /path/to/chessclockdigital_v3.1_Cycles.blend \
        --tex-dir /path/to/textures --out-dir models/clock

Mesh mapping (blend is Z-up with the LCD facing -Y; engine wants Y-up
with the LCD facing +Z, i.e. engine (x,y,z) = (bx, bz, -by)):

  body   = cdtgc_housing + cdtgc_display + cdtgc_legs +
           cdtgc_timer_buttons                       -> digital_body.uvmesh
  rocker = cdtgc_button (top bar, see-saw animated)  -> digital_rocker.uvmesh
  dropped: cdtgc_display_glass (needs alpha the clock path doesn't do),
           cdtgc_display_numbers (baked STATIC digits — the engine draws
           the live 7-segment LCD itself; keeping them would show a
           frozen wrong time under the real one).

Uniform scale to 3.0 engine units wide (the analog clock's width), base
at Y=0 — same anchor the renderer's clock_model expects. The script
prints the fitted display-band centre / tilt / extents: those numbers
are the CHESS_LCD_* defaults baked into board_renderer.cpp, and the
rocker bbox drives the hinge constants there.

Textures (all clocks_Mat; the dropped glass had its own atlas):
  BaseColor x AO -> digital_diffuse.jpg   (AO pre-multiplied: the clock
                                           path has no AO map slot)
  Roughness      -> digital_roughness.jpg
  Metallic       -> digital_metalness.jpg
all downsized 4096 -> 2048. UVs are stored as authored; the engine's
clock shader samples (u, 1-v) which matches Blender's V-up convention.
"""
import argparse
import struct
import sys

import bpy
import numpy as np

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ap = argparse.ArgumentParser()
ap.add_argument("--blend", required=True)
ap.add_argument("--tex-dir", required=True)
ap.add_argument("--out-dir", default="models/clock")
ap.add_argument("--width", type=float, default=3.0,
                help="fitted engine width (analog clock = 3.0)")
ap.add_argument("--body-black", action="store_true",
                help="recolour the maroon body to charcoal black (keeps the "
                     "grey rocker/buttons/LCD and the white printed labels)")
args = ap.parse_args(argv)

BODY = ["cdtgc_housing", "cdtgc_display", "cdtgc_legs", "cdtgc_timer_buttons"]
ROCKER = ["cdtgc_button"]

bpy.ops.wm.open_mainfile(filepath=args.blend)
dg = bpy.context.evaluated_depsgraph_get()


def extract(name):
    """World-space triangles of one object -> (pos, nrm, uv) float arrays."""
    ob = bpy.data.objects[name].evaluated_get(dg)
    me = ob.to_mesh()
    me.calc_loop_triangles()
    if hasattr(me, "calc_normals_split"):
        me.calc_normals_split()
    mw = np.array(ob.matrix_world)
    nrm_mw = np.linalg.inv(mw[:3, :3]).T
    uvl = me.uv_layers.active.data
    P, N, UV = [], [], []
    for tri in me.loop_triangles:
        for li in tri.loops:
            lo = me.loops[li]
            v = me.vertices[lo.vertex_index].co
            p = mw[:3, :3] @ np.array(v) + mw[:3, 3]
            n = nrm_mw @ np.array(lo.normal)
            P.append(p)
            N.append(n)
            UV.append(list(uvl[li].uv))
    ob.to_mesh_clear()
    return np.array(P), np.array(N), np.array(UV)


def to_engine(P, N):
    """Blender Z-up / -Y-front -> engine Y-up / +Z-front (proper rotation)."""
    Pe = np.stack([P[:, 0], P[:, 2], -P[:, 1]], axis=1)
    Ne = np.stack([N[:, 0], N[:, 2], -N[:, 1]], axis=1)
    return Pe, Ne


parts = {}
for name in BODY + ROCKER + ["cdtgc_display"]:
    if name not in parts:
        P, N, UV = extract(name)
        parts[name] = (*to_engine(P, N), UV)

kept = BODY + ROCKER
allP = np.concatenate([parts[n][0] for n in kept])
mn, mx = allP.min(0), allP.max(0)
s = args.width / (mx[0] - mn[0])
# centre X/Z, base at Y=0
cx, cz = (mn[0] + mx[0]) / 2, (mn[2] + mx[2]) / 2
off = np.array([-cx, -mn[1], -cz])


def fit(P):
    return (P + off) * s


def write_uvmesh(path, names):
    P = fit(np.concatenate([parts[n][0] for n in names]))
    N = np.concatenate([parts[n][1] for n in names])
    N = N / (np.linalg.norm(N, axis=1, keepdims=True) + 1e-12)
    UV = np.concatenate([parts[n][2] for n in names])
    flat = np.concatenate([P, N, UV], axis=1).astype(np.float32)
    with open(path, "wb") as fh:
        fh.write(b"UVME")
        fh.write(struct.pack("<I", len(flat)))
        fh.write(flat.tobytes())
    print(f"wrote {path}: {len(flat) // 3} tris, "
          f"bbox {P.min(0).round(3)} .. {P.max(0).round(3)}")
    return P


write_uvmesh(f"{args.out_dir}/digital_body.uvmesh", BODY)
Pr = write_uvmesh(f"{args.out_dir}/digital_rocker.uvmesh", ROCKER)
print(f"ROCKER hinge: py={Pr[:, 1].min():.3f} (bbox base) "
      f"pz={(Pr[:, 2].min() + Pr[:, 2].max()) / 2:.3f} (bbox z-centre)")

# Display band -> LCD constants. Tilt from the band's mean normal
# (engine: en = (0, sin(til), cos(til)), i.e. til = atan2(ny, nz)).
Pd = fit(parts["cdtgc_display"][0])
Nd = parts["cdtgc_display"][1]
n = Nd.mean(0)
n /= np.linalg.norm(n)
import math
til = math.degrees(math.atan2(n[1], n[2]))
dmn, dmx = Pd.min(0), Pd.max(0)
slope_h = math.hypot(dmx[1] - dmn[1], dmx[2] - dmn[2])
print(f"LCD: tilt={til:.1f} deg  centre y={(dmn[1]+dmx[1])/2:.3f} "
      f"z={(dmn[2]+dmx[2])/2:.3f}  half-width={(dmx[0]-dmn[0])/2:.3f} "
      f"slope-height={slope_h:.3f}")

# Textures — PIL ships inside Blender's python.
from PIL import Image

td = args.tex_dir
base = Image.open(f"{td}/chessclockdigital_clocks_Mat_BaseColor.png").convert("RGB")
ao = Image.open(f"{td}/chessclockdigital_clocks_Mat_AO.png").convert("L").resize(base.size)
b = np.asarray(base).astype(np.float32)
if args.body_black:
    # Saturated-red mask -> charcoal, value channel preserved so the
    # baked shading survives. Dilate + blur the mask a touch so bilinear
    # sampling at island borders doesn't leave maroon fringes.
    from PIL import ImageFilter
    r, g, bl = b[..., 0], b[..., 1], b[..., 2]
    mx = b.max(-1)
    sat = (mx - b.min(-1)) / (mx + 1e-6)
    mask = ((sat > 0.25) & (r > np.maximum(g, bl) * 1.15)).astype(np.uint8) * 255
    m = Image.fromarray(mask).filter(ImageFilter.MaxFilter(7)) \
                             .filter(ImageFilter.GaussianBlur(2))
    m = (np.asarray(m).astype(np.float32) / 255.0)[..., None]
    charcoal = (mx * 0.20)[..., None].repeat(3, axis=-1)
    b = b * (1.0 - m) + charcoal * m
    print(f"body recoloured to charcoal: {float(m.mean()) * 100:.1f}% of atlas")
a = (np.asarray(ao).astype(np.float32) / 255.0)[..., None]
diff = Image.fromarray((b * a).clip(0, 255).astype(np.uint8)).resize((2048, 2048), Image.LANCZOS)
diff.save(f"{args.out_dir}/digital_diffuse.jpg", quality=92)
for src, dst in [("Roughness", "digital_roughness.jpg"),
                 ("Metallic", "digital_metalness.jpg")]:
    Image.open(f"{td}/chessclockdigital_clocks_Mat_{src}.png").convert("L") \
        .resize((2048, 2048), Image.LANCZOS) \
        .save(f"{args.out_dir}/{dst}", quality=90)
print("textures written (AO-premultiplied diffuse + roughness + metalness)")
