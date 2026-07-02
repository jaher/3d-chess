#!/usr/bin/env python3
"""Rebuild the datacenter table as exact flat-faced boxes.

The Hunyuan-generated workbench mesh is dimensionally right but its
surfaces carry generation waviness. Instead of smoothing, this re-models
the bench parametrically from measurements taken off the generated mesh
(tools/convert_dc_table.py output): a top slab, a lower shelf, four legs
and four feet — every face perfectly planar with exact axis normals.

Textures are generated too: a small two-tone powder-coat atlas (lighter
top, darker frame) with mild noise so the steel doesn't look sterile.
Colours sampled from the original bake's look.

Writes models/table/dc_table.uvmesh + dc_table_{diffuse,roughness,
metalness}.jpg. Run from the repo root:  python3 tools/rebuild_dc_table.py
"""
import struct

import numpy as np
from PIL import Image

OUT = "models/table"

# ---- measured dimensions (fitted space: X/Z ±7, Y -8.268..0, top-centre) ----
TOP    = (-7.00, 7.00, -1.00, 0.00, -7.00, 7.00)     # x0 x1 y0 y1 z0 z1
SHELF  = (-6.55, 6.55, -6.95, -6.45, -6.40, 6.40)
LEG_Y  = (-7.45, -1.00)
FOOT_Y = (-8.268, -7.45)
LEG_X  = ((-6.60, -5.70), (5.70, 6.60))              # inner/outer per side
LEG_Z  = ((-6.65, -5.25), (5.25, 6.65))
FOOT_MARGIN = 0.06

# ---- atlas layout: two patches with margins --------------------------------
ATLAS = 256
# (u0, v0, u1, v1) in pixels, top patch and frame patch
PATCH_TOP   = (8, 8, 120, 248)
PATCH_FRAME = (136, 8, 248, 248)

def patch_uv(patch, fu, fv):
    """Map face-local (0..1, 0..1) into a patch rect (pixel coords -> uv)."""
    u0, v0, u1, v1 = patch
    u = (u0 + fu * (u1 - u0)) / ATLAS
    v = (v0 + fv * (v1 - v0)) / ATLAS
    return u, v

VERTS = []   # rows of pos3 + nrm3 + uv2

def quad(p00, p10, p11, p01, n, patch):
    """Emit two triangles for a planar quad with outward normal n."""
    uvs = [(0, 0), (1, 0), (1, 1), (0, 1)]
    pts = [p00, p10, p11, p01]
    for tri in ((0, 1, 2), (0, 2, 3)):
        for i in tri:
            u, v = patch_uv(patch, *uvs[i])
            VERTS.append([*pts[i], *n, u, v])

def box(x0, x1, y0, y1, z0, z1, patch):
    quad((x0,y1,z0),(x0,y1,z1),(x1,y1,z1),(x1,y1,z0),(0,1,0),patch)     # top
    quad((x0,y0,z0),(x1,y0,z0),(x1,y0,z1),(x0,y0,z1),(0,-1,0),patch)    # bottom
    quad((x0,y0,z1),(x1,y0,z1),(x1,y1,z1),(x0,y1,z1),(0,0,1),patch)     # front +Z
    quad((x1,y0,z0),(x0,y0,z0),(x0,y1,z0),(x1,y1,z0),(0,0,-1),patch)    # back -Z
    quad((x1,y0,z1),(x1,y0,z0),(x1,y1,z0),(x1,y1,z1),(1,0,0),patch)     # right +X
    quad((x0,y0,z0),(x0,y0,z1),(x0,y1,z1),(x0,y1,z0),(-1,0,0),patch)    # left -X

box(*TOP, PATCH_TOP)
box(*SHELF, PATCH_FRAME)
for lx in LEG_X:
    for lz in LEG_Z:
        box(lx[0], lx[1], LEG_Y[0], LEG_Y[1], lz[0], lz[1], PATCH_FRAME)
        box(lx[0]-FOOT_MARGIN, lx[1]+FOOT_MARGIN, FOOT_Y[0], FOOT_Y[1],
            lz[0]-FOOT_MARGIN, lz[1]+FOOT_MARGIN, PATCH_FRAME)

flat = np.asarray(VERTS, dtype=np.float32)
with open(f"{OUT}/dc_table.uvmesh", "wb") as fh:
    fh.write(b"UVME")
    fh.write(struct.pack("<I", len(flat)))
    fh.write(flat.tobytes())
print(f"wrote {OUT}/dc_table.uvmesh: {len(flat)//3} tris "
      f"({len(flat)} verts, 10 boxes)")

# ---- textures ---------------------------------------------------------------
rng = np.random.default_rng(7)
def patch_fill(img, patch, base, noise):
    u0, v0, u1, v1 = patch
    h, w = v1 - v0, u1 - u0
    n = rng.normal(0, noise, (h, w, 1))
    img[v0:v1, u0:u1] = np.clip(np.asarray(base) + n, 0, 255)

diff = np.full((ATLAS, ATLAS, 3), 128.0)
patch_fill(diff, PATCH_TOP,   (176, 178, 181), 3.0)   # lighter powder-coat top
patch_fill(diff, PATCH_FRAME, (138, 141, 145), 3.5)   # darker frame/legs
Image.fromarray(diff.astype(np.uint8)).save(f"{OUT}/dc_table_diffuse.jpg", quality=95)

rough = np.full((ATLAS, ATLAS, 1), 140.0)
patch_fill(rough, PATCH_TOP, (118,), 4.0)
patch_fill(rough, PATCH_FRAME, (150,), 4.0)
rough = rough[..., 0]
Image.fromarray(rough.astype(np.uint8), "L").save(f"{OUT}/dc_table_roughness.jpg", quality=92)

metal = np.full((ATLAS, ATLAS), 55, dtype=np.uint8)   # powder-coated steel: mostly dielectric
Image.fromarray(metal, "L").save(f"{OUT}/dc_table_metalness.jpg", quality=92)
print("textures written (two-tone powder-coat atlas)")
