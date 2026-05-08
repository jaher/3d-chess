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

Two hands per dial:
- Long minute hand: spans most of the dial radius, its bbox centre
  lies on a radial line from the main dial pivot. Detected by
  high in-plane aspect + reasonable length + radial centre.
- Short sub-dial hand: a smaller hand mounted on its own pivot at
  offset (≈±0.154, -0.137) from each main dial pivot — a small
  auxiliary dial within the main dial face. Detected by the same
  thin-shape heuristic but with a different bbox-centre location.

Outputs:
  models/clock/clock_body.uvmesh         — body minus all needles (in place)
  models/clock/clock_hand_long_l.uvmesh  — left dial's long minute hand
  models/clock/clock_hand_long_r.uvmesh  — right dial's long minute hand
  models/clock/clock_hand_short_l.uvmesh — left sub-dial hand
  models/clock/clock_hand_short_r.uvmesh — right sub-dial hand

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
# Needles sit above the dial face (Z≈0.383) but below the bezel rim.
# 0.385 catches both the short hand (~0.393) and the long hand (~0.422).
NEEDLE_Z_MIN = 0.385


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


def matches_needle(cen, ext, *, length_min, length_max=10.0):
    """A needle: thin shape (in-plane aspect ratio above ~3) above
    the dial face plane, with in-plane length within the requested
    band. Length bands let us tell the long minute hand (≈0.30)
    apart from the short sub-dial hand (≈0.12)."""
    if cen[2] < NEEDLE_Z_MIN:
        return False
    in_plane = sorted([ext[0], ext[1]])
    aspect = in_plane[1] / max(in_plane[0], 1e-9)
    return aspect > 3.0 and length_min < in_plane[1] < length_max


def near_dial(cen, dial_pivot, max_radial=0.25):
    return ((cen[0] - dial_pivot[0]) ** 2
            + (cen[1] - dial_pivot[1]) ** 2) ** 0.5 < max_radial


def hub_center(tris, bbox_ext):
    """For a needle that points roughly along +Y or -Y from a hub
    at one end, find the hub's centre. Take the 30% of vertices
    with the lowest Y (the hand's hub end based on the silhouette
    rendering — the hub is the wider end at the bottom of the
    bbox), and average their positions. Z gets the bbox-centre Z
    so the rotation plane matches the rest of the hand."""
    verts = [v for tri in tris for v in tri]
    verts_sorted = sorted(verts, key=lambda v: v[1])
    take = max(1, int(len(verts_sorted) * 0.30))
    hub = verts_sorted[:take]
    hx = sum(v[0] for v in hub) / len(hub)
    hy = sum(v[1] for v in hub) / len(hub)
    hz = sum(v[2] for v in hub) / len(hub)
    return (hx, hy, hz)


def find_match(comps, tris, dial_pivot, length_min, length_max=10.0):
    """Return the longest connected component that matches the
    needle heuristic for the given length band, or None. Each
    return tuple is (ci, idxs, ext, cen, hub)."""
    candidates = []
    for ci, idxs in enumerate(comps):
        ext, cen, sub = comp_info(tris, idxs)
        if not matches_needle(cen, ext,
                              length_min=length_min,
                              length_max=length_max):
            continue
        if not near_dial(cen, dial_pivot):
            continue
        hub = hub_center(sub, ext)
        candidates.append((ci, idxs, ext, cen, hub))
    candidates.sort(key=lambda x: max(x[2][:2]), reverse=True)
    return candidates[0] if candidates else None


def main():
    body_path = os.path.join(MODELS_DIR, "clock_body.uvmesh")
    print(f"reading {body_path}")
    tris = read_uvmesh(body_path)
    print(f"  {len(tris)} tris total")

    comps = connected_components(tris)
    comps.sort(key=len, reverse=True)
    print(f"  {len(comps)} connected components")

    # Long minute hands: thin radial shapes spanning most of the
    # dial radius (length ≳ 0.20). Short sub-dial hands: thin
    # shapes mounted off-pivot at lower-left of the dial face,
    # length 0.08 < l < 0.15.
    long_l  = find_match(comps, tris, DIAL_PIVOT_L, length_min=0.20)
    long_r  = find_match(comps, tris, DIAL_PIVOT_R, length_min=0.20)
    short_l = find_match(comps, tris, DIAL_PIVOT_L,
                         length_min=0.08, length_max=0.20)
    short_r = find_match(comps, tris, DIAL_PIVOT_R,
                         length_min=0.08, length_max=0.20)

    for label, m in (("long L",  long_l), ("long R",  long_r),
                     ("short L", short_l), ("short R", short_r)):
        if m is None:
            print(f"  WARNING: no match for {label}")
        else:
            print(f"  {label}: comp {m[0]} ({len(m[1])} tris) "
                  f"cen=({m[3][0]:.4f},{m[3][1]:.4f},{m[3][2]:.4f}) "
                  f"hub=({m[4][0]:.4f},{m[4][1]:.4f},{m[4][2]:.4f})")

    if not (long_l and long_r):
        print("ERROR: missing long-hand match; cannot continue.")
        sys.exit(1)

    needle_picks = [m for m in (long_l, long_r, short_l, short_r) if m]
    needle_idx_set = set()
    for m in needle_picks:
        needle_idx_set.update(m[1])

    # All tris NOT chosen as a hand stay in the body.
    keep_idxs = [ti for ti in range(len(tris)) if ti not in needle_idx_set]

    # Write the body-without-hands back in place.
    write_uvmesh(os.path.join(MODELS_DIR, "clock_body.uvmesh"),
                 [tris[i] for i in keep_idxs])

    # Each hand goes to its own file. The renderer rotates each
    # around the corresponding hub.
    pairs = [
        ("clock_hand_long_l",  long_l,
         "long L pivot (main dial)",  DIAL_PIVOT_L),
        ("clock_hand_long_r",  long_r,
         "long R pivot (main dial)",  DIAL_PIVOT_R),
        ("clock_hand_short_l", short_l,
         "short L pivot (sub-dial)",  None),
        ("clock_hand_short_r", short_r,
         "short R pivot (sub-dial)",  None),
    ]
    for name, m, label, override_pivot in pairs:
        if m is None:
            continue
        write_uvmesh(os.path.join(MODELS_DIR, f"{name}.uvmesh"),
                     [tris[i] for i in m[1]])

    # Print pivots for the renderer to hard-code. The long hand's
    # pivot is the main dial centre at the hand's Z stack; the
    # short hand's pivot is the hand's own hub centroid.
    print("\nRenderer pivots (mesh-local):")
    if long_l:
        print(f"  CLOCK_HAND_LONG_L_PIVOT:  ({DIAL_PIVOT_L[0]:.4f}, "
              f"{DIAL_PIVOT_L[1]:.4f}, {long_l[4][2]:.4f})")
    if long_r:
        print(f"  CLOCK_HAND_LONG_R_PIVOT:  ({DIAL_PIVOT_R[0]:.4f}, "
              f"{DIAL_PIVOT_R[1]:.4f}, {long_r[4][2]:.4f})")
    if short_l:
        print(f"  CLOCK_HAND_SHORT_L_PIVOT: ({short_l[4][0]:.4f}, "
              f"{short_l[4][1]:.4f}, {short_l[4][2]:.4f})")
    if short_r:
        print(f"  CLOCK_HAND_SHORT_R_PIVOT: ({short_r[4][0]:.4f}, "
              f"{short_r[4][1]:.4f}, {short_r[4][2]:.4f})")


if __name__ == "__main__":
    main()
