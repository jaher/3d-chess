"""Render side-on silhouettes of the candidate lever components
(at the top of the clock body) so we can confirm which sub-meshes
are the silver press-down levers.

Shows each component projected on the X/Y plane (front view) AND
the X/Z plane (top view), with the body's overall bbox marked
for orientation.
"""

import os
import struct
import sys
from collections import defaultdict
from PIL import Image, ImageDraw

MODELS_DIR = "/home/jaherrero/claude_workspace/3d_chess/models/clock"
OUT_DIR = "/tmp/lever_silhouettes"
SNAP = 1e-4


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
    # We need the body BEFORE the lever extraction, so re-fetch from
    # the original copy if available; otherwise warn.
    body_path = "/tmp/clock_body_original.uvmesh"
    if not os.path.exists(body_path):
        body_path = os.path.join(MODELS_DIR, "clock_body.uvmesh")
    print(f"reading {body_path}")
    tris = read_uvmesh(body_path)
    print(f"  {len(tris)} tris")
    comps = connected_components(tris)
    comps.sort(key=len, reverse=True)
    print(f"  {len(comps)} components")

    # Body overall bbox.
    all_verts = [v for tri in tris for v in tri]
    body_xs = [v[0] for v in all_verts]
    body_ys = [v[1] for v in all_verts]
    body_zs = [v[2] for v in all_verts]
    body_bbox = ((min(body_xs), min(body_ys), min(body_zs)),
                 (max(body_xs), max(body_ys), max(body_zs)))
    print(f"  body bbox: {body_bbox}")

    # Render each component near top of body (Y > 0.95).
    SIZE = 800
    SCALE = 200.0

    def render_xy(name, comp_tris, all_tris_dim=None):
        """Front view: X is horizontal, Y is vertical (Y up)."""
        img = Image.new("RGB", (SIZE, SIZE), (240, 240, 240))
        d = ImageDraw.Draw(img)
        cx_screen, cy_screen = SIZE // 2, SIZE // 2 + 100
        # Body bbox outline (faint)
        if all_tris_dim:
            for v_pair in all_tris_dim:
                pass
        body_x_min, body_x_max = body_bbox[0][0], body_bbox[1][0]
        body_y_min, body_y_max = body_bbox[0][1], body_bbox[1][1]
        d.rectangle(
            (cx_screen + body_x_min * SCALE,
             cy_screen - body_y_max * SCALE,
             cx_screen + body_x_max * SCALE,
             cy_screen - body_y_min * SCALE),
            outline=(180, 180, 180))
        for tri in comp_tris:
            pts = [(cx_screen + v[0] * SCALE,
                    cy_screen - v[1] * SCALE) for v in tri]
            d.polygon(pts, outline=(0, 0, 0), fill=(80, 120, 255))
        return img

    # Only look at components that could be the side-mounted
    # plunger levers: at extreme +/-X (far from the dial pivots).
    for ci, idxs in enumerate(comps):
        if ci > 25:
            break
        verts = [v for ti in idxs for v in tris[ti]]
        xs = [v[0] for v in verts]
        ys = [v[1] for v in verts]
        cx = (min(xs) + max(xs)) * 0.5
        cy = (min(ys) + max(ys)) * 0.5
        if abs(cx) < 1.0:    # only far-X side stuff
            continue
        ext_x = max(xs) - min(xs)
        ext_y = max(ys) - min(ys)
        ext_z = max(v[2] for v in verts) - min(v[2] for v in verts)
        ctris = [tris[ti] for ti in idxs]
        img = render_xy(f"comp{ci}", ctris)
        img.save(os.path.join(OUT_DIR, f"side_comp{ci:02d}.png"))
        print(f"  comp {ci}: {len(idxs)} tris, "
              f"ext ({ext_x:.3f},{ext_y:.3f},{ext_z:.3f}), "
              f"cen ({cx:.4f},{cy:.4f}, {(min(v[2] for v in verts)+max(v[2] for v in verts))*0.5:.4f})")


if __name__ == "__main__":
    main()
