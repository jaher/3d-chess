#!/usr/bin/env python3
"""Convert the Hunyuan chess-clock OBJ into the 3d_chess engine's format.

Steps: load textured OBJ -> drop the generation's ground disc -> split the
top rocker bar from the body -> fit to the analog clock's local bbox
(X +-1.5, Y 0..1.303, Z +-0.477, LCD facing +Z) -> write body/rocker
.uvmesh (UVME: u32 count, then count * [pos3 nrm3 uv2] f32) + textures.

Run with --diag first to print histograms, then with cuts chosen.
"""
import argparse
import struct
import sys

import numpy as np
import trimesh

ap = argparse.ArgumentParser()
ap.add_argument("--diag", action="store_true")
ap.add_argument("--disc-y", type=float, default=None, help="drop faces fully below this y")
ap.add_argument("--rocker-y", type=float, default=None, help="rocker = faces fully above this y")
ap.add_argument("--out-prefix", default="digital")
args = ap.parse_args()

# Parse the OBJ directly: trimesh scrambles the per-corner v/vt pairing on
# this file (rocker sampled pink whatever the V convention). The OBJ is
# machine-written: v, vt, vn, and f v/vt/vn triplets.
Vs, VTs, VNs, corners = [], [], [], []
for line in open("chess_clock_textured.obj"):
    t = line.split()
    if not t: continue
    if t[0] == "v":  Vs.append([float(x) for x in t[1:4]])
    elif t[0] == "vt": VTs.append([float(x) for x in t[1:3]])
    elif t[0] == "vn": VNs.append([float(x) for x in t[1:4]])
    elif t[0] == "f":
        c = []
        for w in t[1:4]:
            p = w.split("/")
            c.append((int(p[0]) - 1,
                      int(p[1]) - 1 if len(p) > 1 and p[1] else -1,
                      int(p[2]) - 1 if len(p) > 2 and p[2] else -1))
        corners.append(c)
Vs, VTs, VNs = map(np.asarray, (Vs, VTs, VNs))
print("obj:", len(Vs), "v,", len(VTs), "vt,", len(VNs), "vn,", len(corners), "faces")
# Expand to unique (v, vt, vn) corner tuples -> indexed arrays
key2idx, V_, UV_, N_ = {}, [], [], []
F = np.empty((len(corners), 3), dtype=np.int64)
for fi, face in enumerate(corners):
    for ci, key in enumerate(face):
        idx = key2idx.get(key)
        if idx is None:
            idx = len(V_)
            key2idx[key] = idx
            V_.append(Vs[key[0]])
            UV_.append(VTs[key[1]] if key[1] >= 0 else (0.0, 0.0))
            N_.append(VNs[key[2]] if key[2] >= 0 else (0.0, 1.0, 0.0))
        F[fi, ci] = idx
V = np.asarray(V_, dtype=np.float64)
UV = np.asarray(UV_, dtype=np.float64)
# UVs stay as authored: the engine's clock shader samples (u, 1-v), which
# is the correct net convention for this bake (verified empirically).
# The OBJ ships no vn at all — compute area-weighted vertex normals.
N = np.zeros_like(V)
e1 = V[F[:, 1]] - V[F[:, 0]]
e2 = V[F[:, 2]] - V[F[:, 0]]
fn = np.cross(e1, e2)          # area-weighted
for c in range(3):
    np.add.at(N, F[:, c], fn)
N /= (np.linalg.norm(N, axis=1, keepdims=True) + 1e-12)
print("expanded:", len(V), "corner-verts,", len(F), "faces; normals computed")
print("bbox min", V.min(0).round(3), "max", V.max(0).round(3))

if args.diag:
    ys = V[:, 1]
    hist, edges = np.histogram(ys, bins=40)
    for h, e in zip(hist, edges):
        bar = "#" * (h // 150)
        print(f"y={e:+.3f} {h:5d} {bar}")
    # XZ radius per y-slice to spot the wide disc
    for lo in np.linspace(ys.min(), ys.max(), 12):
        hi = lo + (ys.max() - ys.min()) / 12
        m = (ys >= lo) & (ys < hi)
        if m.sum() == 0:
            continue
        r = np.sqrt(V[m, 0] ** 2 + V[m, 2] ** 2).max()
        print(f"slice y[{lo:+.3f},{hi:+.3f}) n={m.sum():5d} max_xz_r={r:.3f}")
    sys.exit(0)

face_y = V[F, 1]              # (nf, 3)
keep = np.ones(len(F), bool)
if args.disc_y is not None:
    keep &= ~(face_y.max(axis=1) < args.disc_y)
Fk = F[keep]
print(f"after disc cut: {len(Fk)} faces")

# fit to analog clock local bbox using the KEPT geometry only
used = np.unique(Fk)
mn, mx = V[used].min(0), V[used].max(0)
target_min = np.array([-1.5, 0.0, -0.477])
target_max = np.array([ 1.5, 1.303, 0.477])
scale = (target_max - target_min) / (mx - mn)
Vt = (V - mn) * scale + target_min
# normals: renormalise under non-uniform scale (n' = n / s)
Nt = N / scale
Nt /= (np.linalg.norm(Nt, axis=1, keepdims=True) + 1e-12)
print("fitted bbox", Vt[used].min(0).round(3), Vt[used].max(0).round(3),
      "scale", scale.round(3))

rocker = np.zeros(len(Fk), bool)
if args.rocker_y is not None:
    # rocker cut in FITTED coordinates
    rocker = Vt[Fk, 1].min(axis=1) > args.rocker_y
print(f"rocker faces: {rocker.sum()}  body faces: {(~rocker).sum()}")

def write_uvmesh(path, faces):
    flat = np.empty((len(faces) * 3, 8), dtype=np.float32)
    k = 0
    for tri in faces:
        for vi in tri:
            flat[k, 0:3] = Vt[vi]
            flat[k, 3:6] = Nt[vi]
            flat[k, 6:8] = UV[vi]
            k += 1
    with open(path, "wb") as fh:
        fh.write(b"UVME")
        fh.write(struct.pack("<I", len(flat)))
        fh.write(flat.tobytes())
    print(f"wrote {path}: {len(faces)} tris")

write_uvmesh(f"{args.out_prefix}_body.uvmesh", Fk[~rocker])
write_uvmesh(f"{args.out_prefix}_rocker.uvmesh", Fk[rocker])

# Textures. Two bake artifacts to clean, both red-bleed from the
# multiview projection:
#  1. The white rocker's region is painted pink-ish white -> neutralise
#     that region (its UV mask) to grey-luma so the plastic reads white.
#  2. Island borders carry pink fringe texels (high R, mid B). Inpaint
#     them from non-pink neighbours — but spare the true DGT red, which
#     has LOW blue.
from PIL import Image, ImageDraw
from scipy import ndimage
diff = np.asarray(Image.open("chess_clock_textured.jpg").convert("RGB"),
                  dtype=np.uint8).copy()
H2, W2 = diff.shape[:2]
# rocker UV mask (engine samples (u, 1-v): rasterise in that space)
rmask_img = Image.new("L", (W2, H2), 0)
dr = ImageDraw.Draw(rmask_img)
# neutralise the rocker AND the body's top strip (white plastic on the
# real DGT; both catch red projection bleed)
top_strip = Vt[Fk, 1].min(axis=1) > 0.85
for tri in Fk[rocker | top_strip]:
    pts = [(UV[vv, 0] * (W2 - 1), (1.0 - UV[vv, 1]) * (H2 - 1)) for vv in tri]
    dr.polygon(pts, fill=255)
rmask = np.asarray(rmask_img) > 0
rmask = ndimage.binary_dilation(rmask, iterations=3)
luma = (0.3 * diff[..., 0] + 0.5 * diff[..., 1] + 0.2 * diff[..., 2]).astype(np.uint8)
for c in range(3):
    diff[..., c][rmask] = np.clip(luma[rmask].astype(int) + 40, 0, 235)
print(f"rocker region neutralised: {rmask.mean()*100:.1f}% of atlas")
# End caps (|x| > 1.30 below the white top): solid red on the real DGT,
# but the bake left red/white mosaic there that bilinear-averages to pink.
# Snap non-dark texels in those regions to the body red.
emask_img = Image.new("L", (W2, H2), 0)
de = ImageDraw.Draw(emask_img)
endcap = ((np.abs(Vt[Fk, 0]).min(axis=1) > 1.12) & (Vt[Fk, 1].min(axis=1) < 0.9)) | \
         ((np.abs(Vt[Fk, 0]).min(axis=1) > 1.00) & (Vt[Fk, 1].max(axis=1) < 0.6) &
          (Vt[Fk, 2].min(axis=1) > 0.05))
for tri in Fk[endcap]:
    pts = [(UV[vv, 0] * (W2 - 1), (1.0 - UV[vv, 1]) * (H2 - 1)) for vv in tri]
    de.polygon(pts, fill=255)
emask = ndimage.binary_dilation(np.asarray(emask_img) > 0, iterations=3) & ~rmask
bright = (0.3*diff[...,0] + 0.5*diff[...,1] + 0.2*diff[...,2]) > 70
sel = emask & bright
diff[..., 0][sel] = 150; diff[..., 1][sel] = 25; diff[..., 2][sel] = 45
print(f"end caps reddened: {sel.mean()*100:.2f}% of atlas")
r = diff[..., 0].astype(int); g = diff[..., 1].astype(int); b = diff[..., 2].astype(int)
pink = (r > 140) & (g < r - 40) & (b > 80) & ~rmask
print(f"pink fringe texels: {pink.mean()*100:.2f}%")
idx = ndimage.distance_transform_edt(pink, return_distances=False, return_indices=True)
diff = diff[idx[0], idx[1]]
# Final pass: red/white MOSAIC zones (bake noise) bilinear-average to pink
# in-engine no matter what individual texels say — homogenise each mosaic
# zone to its local-majority colour.
r2 = diff[..., 0].astype(int); g2 = diff[..., 1].astype(int); b2 = diff[..., 2].astype(int)
redc  = (r2 > 120) & (g2 < 80) & (b2 < 95)
lum2  = 0.3 * r2 + 0.5 * g2 + 0.2 * b2
whitec = (lum2 > 140) & (np.abs(r2 - g2) < 45) & (np.abs(g2 - b2) < 45)
def boxsum(m, k):
    c = np.cumsum(np.cumsum(m.astype(np.int32), 0), 1)
    c = np.pad(c, ((1, 0), (1, 0)))
    Hh, Ww = m.shape
    i0 = np.clip(np.arange(Hh) - k, 0, Hh); i1 = np.clip(np.arange(Hh) + k + 1, 0, Hh)
    j0 = np.clip(np.arange(Ww) - k, 0, Ww); j1 = np.clip(np.arange(Ww) + k + 1, 0, Ww)
    return c[np.ix_(i1, j1)] - c[np.ix_(i0, j1)] - c[np.ix_(i1, j0)] + c[np.ix_(i0, j0)]
nr = boxsum(redc, 4); nw = boxsum(whitec, 4)
mosaic = (nr > 6) & (nw > 6)
sr = mosaic & (nr >= nw); sw = mosaic & (nw > nr)
diff[..., 0][sr] = 150; diff[..., 1][sr] = 25; diff[..., 2][sr] = 45
for c in range(3):
    diff[..., c][sw] = 210
print(f"mosaic homogenised: {mosaic.mean()*100:.2f}%")
Image.fromarray(diff).save(f"{args.out_prefix}_diffuse.jpg", quality=94)
Image.open("chess_clock_textured_roughness.jpg").convert("L").save(f"{args.out_prefix}_roughness.jpg", quality=92)
Image.open("chess_clock_textured_metallic.jpg").convert("L").save(f"{args.out_prefix}_metalness.jpg", quality=92)
print("textures written (rocker-neutralised + fringe-inpainted)")
