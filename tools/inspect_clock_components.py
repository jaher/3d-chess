"""Print the radial distance histogram of vertices in components
near the dial pivot, to figure out which buried geometry is the
SHORT needle (visible in the rendered clock alongside the long
minute hand). A short hand pointing radially from the pivot will
have a distance histogram concentrated between 0 and its length;
a hub cap will have all vertices within a tiny radius of 0.
"""

import os
import struct
from collections import defaultdict

MODELS_DIR = "/home/jaherrero/claude_workspace/3d_chess/models/clock"
SNAP = 1e-4

DIAL_R = ( 0.6422, 0.6999)
DIAL_L = (-0.6456, 0.6999)


def read_uvmesh(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"UVME":
        raise ValueError("bad magic")
    n = struct.unpack("<I", data[4:8])[0]
    verts = []
    off = 8
    stride = 32
    for _ in range(n):
        verts.append(struct.unpack("<8f", data[off:off + stride]))
        off += stride
    return [verts[i:i + 3] for i in range(0, n, 3)]


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


def hist(values, bins, lo, hi):
    h = [0] * bins
    for v in values:
        if v < lo or v >= hi:
            continue
        b = int((v - lo) / (hi - lo) * bins)
        h[min(b, bins - 1)] += 1
    return h


def main():
    tris = read_uvmesh(os.path.join(MODELS_DIR, "clock_body.uvmesh"))
    comps = connected_components(tris)
    comps.sort(key=len, reverse=True)
    print(f"{len(comps)} components, {len(tris)} tris\n")

    # For each dial, show distance-from-pivot stats for every
    # component whose bbox CENTRE lies within 0.20 of the pivot in
    # the XY plane and is on the +Z side of the dial face.
    for label, pivot in (("LEFT", DIAL_L), ("RIGHT", DIAL_R)):
        print(f"== Components near {label} dial pivot {pivot} ==")
        for ci, idxs in enumerate(comps):
            verts = [v for ti in idxs for v in tris[ti]]
            xs = [v[0] for v in verts]
            ys = [v[1] for v in verts]
            zs = [v[2] for v in verts]
            cx = (min(xs) + max(xs)) * 0.5
            cy = (min(ys) + max(ys)) * 0.5
            cz = (min(zs) + max(zs)) * 0.5
            dxy = ((cx - pivot[0]) ** 2 + (cy - pivot[1]) ** 2) ** 0.5
            if dxy > 0.30 or cz < 0.38:
                continue
            # Distance of every vertex from the dial pivot in XY.
            dists = [((v[0] - pivot[0]) ** 2
                      + (v[1] - pivot[1]) ** 2) ** 0.5
                     for v in verts]
            dmax = max(dists)
            dmean = sum(dists) / len(dists)
            ext_x = max(xs) - min(xs)
            ext_y = max(ys) - min(ys)
            ext_z = max(zs) - min(zs)
            print(f"  comp {ci:2d}: {len(idxs):4d} tris  "
                  f"ext ({ext_x:.3f},{ext_y:.3f},{ext_z:.3f})  "
                  f"cen ({cx:.4f},{cy:.4f},{cz:.4f})  "
                  f"d_to_pivot={dxy:.3f}  d_max={dmax:.3f}  "
                  f"d_mean={dmean:.3f}")
        print()


if __name__ == "__main__":
    main()
