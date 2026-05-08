"""For each connected component near a dial pivot, draw the
component's silhouette as seen from the front of the dial (project
all vertices onto the local XY plane around the pivot). Saves a
PNG per component so I can eyeball which one looks like a clock
hand.
"""

import os
import struct
from collections import defaultdict
from PIL import Image, ImageDraw

MODELS_DIR = "/home/jaherrero/claude_workspace/3d_chess/models/clock"
OUT_DIR = "/tmp/clock_silhouettes"
SNAP = 1e-4

DIAL_R = ( 0.6422, 0.6999)
DIAL_L = (-0.6456, 0.6999)


def read_uvmesh(path):
    with open(path, "rb") as f:
        data = f.read()
    n = struct.unpack("<I", data[4:8])[0]
    verts = []
    off = 8
    for _ in range(n):
        verts.append(struct.unpack("<8f", data[off:off + 32]))
        off += 32
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

    for tris_on_edge in edge_to_tris.values():
        for i in range(1, len(tris_on_edge)):
            ra, rb = find(tris_on_edge[0]), find(tris_on_edge[i])
            if ra != rb:
                parent[ra] = rb

    groups = defaultdict(list)
    for ti in range(len(tris)):
        groups[find(ti)].append(ti)
    return list(groups.values())


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    tris = read_uvmesh(os.path.join(MODELS_DIR, "clock_body.uvmesh"))
    comps = connected_components(tris)
    comps.sort(key=len, reverse=True)
    print(f"{len(comps)} components")

    SIZE = 600
    SCALE = 800.0   # 1.0 mesh unit ≈ this many px

    # Also load the long-hand mesh for reference.
    long_l = read_uvmesh(os.path.join(MODELS_DIR, "clock_hand_l.uvmesh"))
    long_r = read_uvmesh(os.path.join(MODELS_DIR, "clock_hand_r.uvmesh"))

    def render_component(name, comp_tris, pivot, size=SIZE, scale=SCALE):
        img = Image.new("RGB", (size, size), (240, 240, 240))
        d = ImageDraw.Draw(img)
        # Draw a faint pivot dot + circle marking the dial radius.
        cx, cy = size // 2, size // 2
        d.ellipse((cx - 4, cy - 4, cx + 4, cy + 4), outline=(255, 0, 0))
        # Dial face is ~0.46 radius.
        r_face = int(0.46 * scale)
        d.ellipse((cx - r_face, cy - r_face, cx + r_face, cy + r_face),
                  outline=(180, 180, 180))
        for tri in comp_tris:
            pts = []
            for v in tri:
                # Project around dial pivot.
                px = (v[0] - pivot[0]) * scale + cx
                # Y is up in mesh; flip for image coordinates.
                py = -(v[1] - pivot[1]) * scale + cy
                pts.append((px, py))
            d.polygon(pts, outline=(0, 0, 0), fill=(80, 120, 255))
        return img

    for label, pivot in (("L", DIAL_L), ("R", DIAL_R)):
        # Filter to components whose CENTRE is within 0.30 of this pivot.
        for ci, idxs in enumerate(comps):
            verts = [v for ti in idxs for v in tris[ti]]
            xs = [v[0] for v in verts]
            ys = [v[1] for v in verts]
            zs = [v[2] for v in verts]
            cz = (min(zs) + max(zs)) * 0.5
            cx0 = (min(xs) + max(xs)) * 0.5
            cy0 = (min(ys) + max(ys)) * 0.5
            d_to_pivot = ((cx0 - pivot[0]) ** 2
                          + (cy0 - pivot[1]) ** 2) ** 0.5
            # Skip the bezel (huge), the body shell, and far stuff.
            if len(idxs) > 1500 or d_to_pivot > 0.30 or cz < 0.36:
                continue
            ctris = [tris[ti] for ti in idxs]
            img = render_component(f"{label}_comp{ci}", ctris, pivot)
            img.save(os.path.join(OUT_DIR, f"dial_{label}_comp{ci:02d}.png"))
            print(f"  wrote dial_{label}_comp{ci:02d}.png "
                  f"({len(idxs)} tris, Z={cz:.4f})")
        # Also render the existing long hand for reference.
        long_tris = long_r if label == "R" else long_l
        img = render_component(f"{label}_long_hand", long_tris, pivot)
        img.save(os.path.join(OUT_DIR, f"dial_{label}_long_hand_REF.png"))
        print(f"  wrote dial_{label}_long_hand_REF.png ({len(long_tris)} tris)")


if __name__ == "__main__":
    main()
