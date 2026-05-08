"""Split clock_body.uvmesh into a static body + animatable needles.

The Sketchfab "Cursore" sub-meshes turned out to be the textured
dial faces (numbers + knight icon), NOT the needles. The actual
needles are 3D geometry buried inside Clock.001 (the body mesh).
This script finds them by connected-components analysis and writes
them out as their own uvmesh files so the renderer can rotate
each needle independently around its dial pivot.

This script REWRITES models/clock/clock_body.uvmesh in place,
replacing it with the body-minus-needles version. The hands are
written to new files. Run this AFTER convert_clock_uvmesh.py
regenerates the body from the Sketchfab OBJ.

Outputs:
  models/clock/clock_body.uvmesh   — body minus needles (in place)
  models/clock/clock_hand_l.uvmesh — left dial's needle
  models/clock/clock_hand_r.uvmesh — right dial's needle

Detection heuristic for a needle: a connected component whose
in-plane bbox center is within ~0.05 of a dial pivot in (X, Y),
whose Z lies above the dial face (Z > 0.40 vs face Z=0.384), and
whose in-plane aspect ratio is high (long thin shape). The two
matching components per dial are sorted by length so the longer
one becomes the visible "long" hand. (If only one shows up per
dial, that's fine — the user can review and we can refine later.)

The script also prints the pivot Z height for each needle so the
renderer can use the right per-axis rotation pivot point.
"""

import os
import struct
import sys
from collections import defaultdict

MODELS_DIR = "/home/jaherrero/claude_workspace/3d_chess/models/clock"
SNAP = 1e-4

# Dial-pivot positions in mesh-local space (computed earlier from
# the bbox centres of clock_dial_{l,r}.uvmesh).
DIAL_PIVOT_L = (-0.6456, 0.6999)
DIAL_PIVOT_R = ( 0.6422, 0.6999)
PIVOT_RADIUS = 0.05         # how close a needle's centre must be in XY
NEEDLE_Z_MIN = 0.40         # needles sit above the dial face plane


def read_uvmesh(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"UVME":
        raise ValueError(f"{path}: bad magic")
    n = struct.unpack("<I", data[4:8])[0]
    verts = []
    off = 8
    stride = 8 * 4
    for _ in range(n):
        v = struct.unpack("<8f", data[off:off + stride])
        verts.append(v)
        off += stride
    if n % 3 != 0:
        raise ValueError(f"{path}: vertex count {n} not divisible by 3")
    return [verts[i:i + 3] for i in range(0, n, 3)]


def write_uvmesh(path, tris):
    pieces = [v for tri in tris for v in tri]
    with open(path, "wb") as f:
        f.write(b"UVME")
        f.write(struct.pack("<I", len(pieces)))
        for v in pieces:
            f.write(struct.pack("<8f", *v))
    print(f"  wrote {path}: {len(tris)} tris")


def snap(p):
    return (round(p[0] / SNAP), round(p[1] / SNAP), round(p[2] / SNAP))


def connected_components(tris):
    edge_to_tris = defaultdict(list)
    for ti, tri in enumerate(tris):
        s = [snap(v[:3]) for v in tri]
        for a, b in [(0, 1), (1, 2), (2, 0)]:
            edge = tuple(sorted([s[a], s[b]]))
            edge_to_tris[edge].append(ti)

    parent = list(range(len(tris)))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb

    for tris_on_edge in edge_to_tris.values():
        for i in range(1, len(tris_on_edge)):
            union(tris_on_edge[0], tris_on_edge[i])

    groups = defaultdict(list)
    for ti in range(len(tris)):
        groups[find(ti)].append(ti)
    return list(groups.values())


def comp_info(tris, idxs):
    sub = [tris[i] for i in idxs]
    xs = [v[0] for tri in sub for v in tri]
    ys = [v[1] for tri in sub for v in tri]
    zs = [v[2] for tri in sub for v in tri]
    mn = (min(xs), min(ys), min(zs))
    mx = (max(xs), max(ys), max(zs))
    ext = (mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2])
    cen = ((mn[0] + mx[0]) * 0.5,
           (mn[1] + mx[1]) * 0.5,
           (mn[2] + mx[2]) * 0.5)
    return ext, cen, sub


def near_pivot(cen, pivot):
    return ((cen[0] - pivot[0]) ** 2
            + (cen[1] - pivot[1]) ** 2) ** 0.5 < PIVOT_RADIUS


def matches_needle(cen, ext):
    """Needle: thin (high aspect in XY), at the dial face's Z stack
    (above the disc), and bounding-box-centred ROUGHLY along a radial
    line from the dial pivot. Detection is permissive — let the
    survey output guide what to extract per clock."""
    if cen[2] < NEEDLE_Z_MIN:
        return False
    in_plane = sorted([ext[0], ext[1]])
    aspect = in_plane[1] / max(in_plane[0], 1e-9)
    return aspect > 4.0 and in_plane[1] > 0.10


def belongs_to_dial(cen, dial_x):
    """A needle's bbox centre falls along the radial direction from
    the dial pivot. If the needle points straight down (-Y), the
    centre's X equals the dial pivot's X within ~half the needle's
    width. If it points diagonally, X drifts. Use a generous radial
    test: the needle's centre must be within ±0.25 of the dial X."""
    return abs(cen[0] - dial_x) < 0.25


def main():
    body_path = os.path.join(MODELS_DIR, "clock_body.uvmesh")
    print(f"reading {body_path}")
    tris = read_uvmesh(body_path)
    print(f"  {len(tris)} tris total")

    comps = connected_components(tris)
    comps.sort(key=len, reverse=True)
    print(f"  {len(comps)} connected components")

    needle_idxs_l = []   # candidate components for left needle
    needle_idxs_r = []
    keep_idxs = []       # everything that's NOT a needle

    for ci, idxs in enumerate(comps):
        ext, cen, sub = comp_info(tris, idxs)
        is_needle = matches_needle(cen, ext)
        if is_needle and belongs_to_dial(cen, DIAL_PIVOT_L[0]):
            needle_idxs_l.append((ci, idxs, ext, cen))
            print(f"  → LEFT needle candidate (comp {ci}): "
                  f"len={max(ext[:2]):.3f} cen={cen}")
        elif is_needle and belongs_to_dial(cen, DIAL_PIVOT_R[0]):
            needle_idxs_r.append((ci, idxs, ext, cen))
            print(f"  → RIGHT needle candidate (comp {ci}): "
                  f"len={max(ext[:2]):.3f} cen={cen}")
        else:
            keep_idxs.extend(idxs)

    if not needle_idxs_l or not needle_idxs_r:
        print("ERROR: didn't find one needle per dial.")
        print(f"  left={len(needle_idxs_l)}, right={len(needle_idxs_r)}")
        sys.exit(1)

    # Pick the LONGEST candidate per side as the visible needle. If
    # multiple were detected (e.g. minute + second hands), the
    # shorter ones go BACK to the static body so they at least
    # render correctly even if they don't animate yet.
    needle_idxs_l.sort(key=lambda x: max(x[2][:2]), reverse=True)
    needle_idxs_r.sort(key=lambda x: max(x[2][:2]), reverse=True)
    chosen_l = needle_idxs_l[0]
    chosen_r = needle_idxs_r[0]
    for extra in needle_idxs_l[1:] + needle_idxs_r[1:]:
        keep_idxs.extend(extra[1])
        print(f"  (extra needle comp {extra[0]} len={max(extra[2][:2]):.3f} "
              f"sent back to static body)")

    print(f"\nchosen LEFT  needle: comp {chosen_l[0]} "
          f"({len(chosen_l[1])} tris, length {max(chosen_l[2][:2]):.3f})")
    print(f"chosen RIGHT needle: comp {chosen_r[0]} "
          f"({len(chosen_r[1])} tris, length {max(chosen_r[2][:2]):.3f})")

    # Write outputs. Rewrites clock_body.uvmesh in place so the
    # runtime asset list stays clean (the needles were the only
    # reason to keep the bigger original). Re-running this script
    # against an already-split body will fail the "didn't find one
    # needle per dial" check above — that's intentional, it means
    # the file has already been processed.
    write_uvmesh(os.path.join(MODELS_DIR, "clock_body.uvmesh"),
                 [tris[i] for i in keep_idxs])
    write_uvmesh(os.path.join(MODELS_DIR, "clock_hand_l.uvmesh"),
                 [tris[i] for i in chosen_l[1]])
    write_uvmesh(os.path.join(MODELS_DIR, "clock_hand_r.uvmesh"),
                 [tris[i] for i in chosen_r[1]])

    # Print pivots for the renderer (Z is the needle's bbox-centre Z).
    print("\nRenderer pivots (mesh-local):")
    print(f"  CLOCK_HAND_L_PIVOT: ({DIAL_PIVOT_L[0]:.4f}, "
          f"{DIAL_PIVOT_L[1]:.4f}, {chosen_l[3][2]:.4f})")
    print(f"  CLOCK_HAND_R_PIVOT: ({DIAL_PIVOT_R[0]:.4f}, "
          f"{DIAL_PIVOT_R[1]:.4f}, {chosen_r[3][2]:.4f})")


if __name__ == "__main__":
    main()
