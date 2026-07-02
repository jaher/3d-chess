#!/usr/bin/env python3
"""Convert the Hunyuan datacenter-table OBJ into the engine's format.

Sibling of convert_digital_clock.py (same direct OBJ parsing, computed
normals, ground-disc trim, V handled by the engine shader's (u, 1-v)):
fits the generated steel workbench to the MEDIEVAL TABLE's exact local
bbox — X/Z ±7, Y -8.268..0, top-centre origin — so the board, pieces,
captured-piece slots and clock all sit identically on it.

Run on the box with the Hunyuan outputs:
    python3 convert_dc_table.py --diag
    python3 convert_dc_table.py --disc-y <cut>
"""
import argparse
import struct
import sys

import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("--diag", action="store_true")
ap.add_argument("--disc-y", type=float, default=None)
ap.add_argument("--obj", default="dc_table_textured.obj")
ap.add_argument("--out-prefix", default="dc_table")
args = ap.parse_args()

Vs, VTs, corners = [], [], []
for line in open(args.obj):
    t = line.split()
    if not t:
        continue
    if t[0] == "v":
        Vs.append([float(x) for x in t[1:4]])
    elif t[0] == "vt":
        VTs.append([float(x) for x in t[1:3]])
    elif t[0] == "f":
        c = []
        for w in t[1:4]:
            p = w.split("/")
            c.append((int(p[0]) - 1, int(p[1]) - 1 if len(p) > 1 and p[1] else -1))
        corners.append(c)
Vs, VTs = np.asarray(Vs), np.asarray(VTs)
print("obj:", len(Vs), "v,", len(VTs), "vt,", len(corners), "faces")

key2idx, V_, UV_ = {}, [], []
F = np.empty((len(corners), 3), dtype=np.int64)
for fi, face in enumerate(corners):
    for ci, key in enumerate(face):
        idx = key2idx.get(key)
        if idx is None:
            idx = len(V_)
            key2idx[key] = idx
            V_.append(Vs[key[0]])
            UV_.append(VTs[key[1]] if key[1] >= 0 else (0.0, 0.0))
        F[fi, ci] = idx
V = np.asarray(V_, dtype=np.float64)
UV = np.asarray(UV_, dtype=np.float64)

if args.diag:
    ys = V[:, 1]
    for lo in np.linspace(ys.min(), ys.max(), 14):
        hi = lo + (ys.max() - ys.min()) / 14
        m = (ys >= lo) & (ys < hi)
        if m.sum() == 0:
            continue
        r = np.sqrt(V[m, 0] ** 2 + V[m, 2] ** 2).max()
        print(f"slice y[{lo:+.3f},{hi:+.3f}) n={m.sum():6d} max_xz_r={r:.3f}")
    sys.exit(0)

face_y = V[F, 1]
keep = np.ones(len(F), bool)
if args.disc_y is not None:
    keep &= ~(face_y.max(axis=1) < args.disc_y)
Fk = F[keep]
print(f"after disc cut: {len(Fk)} faces")

# --- Taubin smoothing (shrink-free Laplacian) ---------------------------
# Hunyuan surfaces carry high-frequency waviness that reads as "bumpy"
# under lighting. Smooth positions over a POSITION-WELDED adjacency (UV
# seams split vertices; welding the graph keeps seams crack-free), then
# fit + recompute normals from the smoothed geometry.
weld = {}
weld_id = np.empty(len(V), dtype=np.int64)
for i, p in enumerate(np.round(V, 6)):
    key = (p[0], p[1], p[2])
    weld_id[i] = weld.setdefault(key, len(weld))
nw = len(weld)
# adjacency accumulation buffers in weld space
def smooth_pass(P, lam):
    acc = np.zeros((nw, 3))
    cnt = np.zeros(nw)
    wf = weld_id[F]
    for a, b in ((0, 1), (1, 2), (2, 0)):
        np.add.at(acc, wf[:, a], P[wf[:, b]])
        np.add.at(cnt, wf[:, a], 1)
        np.add.at(acc, wf[:, b], P[wf[:, a]])
        np.add.at(cnt, wf[:, b], 1)
    avg = acc / np.maximum(cnt, 1)[:, None]
    return P + lam * (avg - P)
Pw = np.zeros((nw, 3))
Pw[weld_id] = V
for _ in range(25):                      # Taubin: inflate-deflate pairs
    Pw = smooth_pass(Pw, 0.5)
    Pw = smooth_pass(Pw, -0.53)
V = Pw[weld_id]
print("taubin smoothing applied (25 passes)")

used = np.unique(Fk)
mn, mx = V[used].min(0), V[used].max(0)
# medieval table local box: top-centre origin
target_min = np.array([-7.0, -8.268, -7.0])
target_max = np.array([7.0, 0.0, 7.0])
scale = (target_max - target_min) / (mx - mn)
Vt = (V - mn) * scale + target_min
print("fitted bbox", Vt[used].min(0).round(3), Vt[used].max(0).round(3),
      "scale", scale.round(3))

# area-weighted normals (the OBJ ships none)
N = np.zeros_like(V)
e1 = Vt[F[:, 1]] - Vt[F[:, 0]]
e2 = Vt[F[:, 2]] - Vt[F[:, 0]]
fn = np.cross(e1, e2)
for c in range(3):
    np.add.at(N, F[:, c], fn)
N /= (np.linalg.norm(N, axis=1, keepdims=True) + 1e-12)

def write_uvmesh(path, faces):
    flat = np.empty((len(faces) * 3, 8), dtype=np.float32)
    k = 0
    for tri in faces:
        for vi in tri:
            flat[k, 0:3] = Vt[vi]
            flat[k, 3:6] = N[vi]
            flat[k, 6:8] = UV[vi]
            k += 1
    with open(path, "wb") as fh:
        fh.write(b"UVME")
        fh.write(struct.pack("<I", len(flat)))
        fh.write(flat.tobytes())
    print(f"wrote {path}: {len(faces)} tris")

write_uvmesh(f"{args.out_prefix}.uvmesh", Fk)

from PIL import Image
base = args.obj.replace(".obj", "")
Image.open(base + ".jpg").save(f"{args.out_prefix}_diffuse.jpg", quality=94)
# glossier steel: scale the (noisy) roughness bake down
rough = np.asarray(Image.open(base + "_roughness.jpg").convert("L"), dtype=np.float64)
Image.fromarray((rough * 0.45).clip(0, 255).astype(np.uint8)).save(
    f"{args.out_prefix}_roughness.jpg", quality=92)
Image.open(base + "_metallic.jpg").convert("L").save(f"{args.out_prefix}_metalness.jpg", quality=92)
print("textures written")
