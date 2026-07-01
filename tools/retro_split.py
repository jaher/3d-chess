#!/usr/bin/env python3
"""Split a RetroPC piece mesh into a static base + an animated moving part.

The retro pieces are single meshes; to animate a sub-part (the rook's fan
blades, the bishop's joystick, …) we segment the mesh by connected component
and write two .uvmesh files that the renderer draws as base + moving
(board_renderer.cpp's RetroAnim path). Both parts are normalised IDENTICALLY —
using the WHOLE piece's world bbox, exactly like tools/convert_retro_pieces.py —
so they line up when drawn together and the animation pivot is in that same
normalised space (≈ [-1, 1]).

Usage as a module (what per-piece agents call):

    import retro_split as rs
    P = rs.load_piece(mesh_idx=31, node_name="Object_66")   # white Rook
    for i, c in enumerate(P.components):                     # inspect
        print(i, c.n, c.centroid.round(2), c.radius.round(2), c.bbox_size.round(2))
    moving = {i for i, c in enumerate(P.components)
              if 0.25 < c.centroid[1] < 0.40 and c.radius > 0.10}   # fan blades
    rs.write_split("Rook", P, moving, out="models/retro")

`load_piece` gives, per component: vertex count `.n`, `.centroid`, `.bbox_min/max`,
`.bbox_size`, and `.radius` (max XZ distance from the Y axis) — enough to pick a
sub-part by region. `write_split` writes <Piece>_base.uvmesh + _moving.uvmesh.
Also mirror them into models-web-packed/retro for the web build, and add a line
to models/retro/anim.cfg (and the web copy).
"""
import os, struct, json
import numpy as np

GLB = os.path.expanduser("~/retro_chess/retropc_chess.glb")

with open(GLB, "rb") as f:
    f.read(12)
    clen, ct = struct.unpack("<I4s", f.read(8)); _JS = json.loads(f.read(clen))
    blen, bt = struct.unpack("<I4s", f.read(8)); _BIN = f.read(blen)
_NODES = _JS["nodes"]; _ACCS = _JS["accessors"]; _BVS = _JS["bufferViews"]; _MESHES = _JS["meshes"]
_name2idx = {n.get("name"): i for i, n in enumerate(_NODES)}
_parent = {}
for _i, _n in enumerate(_NODES):
    for _c in _n.get("children", []): _parent[_c] = _i

def _node_matrix(n):
    nd = _NODES[n]
    if "matrix" in nd: return np.array(nd["matrix"], dtype=np.float64).reshape(4, 4).T
    M = np.eye(4)
    if "scale" in nd: M = np.diag(list(nd["scale"]) + [1.0]) @ M
    if "rotation" in nd:
        x, y, z, w = nd["rotation"]
        R = np.array([[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
                      [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
                      [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]])
        T = np.eye(4); T[:3, :3] = R; M = T @ M
    if "translation" in nd:
        T = np.eye(4); T[:3, 3] = nd["translation"]; M = T @ M
    return M

def _world_matrix(n):
    M = np.eye(4); chain = []; i = n
    while i is not None: chain.append(i); i = _parent.get(i)
    for i in reversed(chain): M = M @ _node_matrix(i)
    return M

_DT = {5126: np.float32, 5123: np.uint16, 5125: np.uint32, 5121: np.uint8}
_NC = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}
def _read_acc(ai):
    a = _ACCS[ai]; bv = _BVS[a["bufferView"]]
    base = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    nc = _NC[a["type"]]; cnt = a["count"]
    raw = np.frombuffer(_BIN, dtype=_DT[a["componentType"]], count=cnt * nc, offset=base)
    return raw.reshape(cnt, nc).astype(np.float64)


class Component:
    __slots__ = ("ids", "n", "centroid", "bbox_min", "bbox_max", "bbox_size", "radius")

class Piece:
    __slots__ = ("pos", "nrm", "uv", "tris", "components", "center", "scale", "tri_comp")


def load_piece(mesh_idx, node_name):
    """Return a Piece: normalised verts/normals/uv, triangles, and per-component
    stats (all in the normalised [-1,1]-ish space the renderer uses)."""
    nidx = _name2idx[node_name]
    W = _world_matrix(nidx); R = W[:3, :3]
    prim = _MESHES[mesh_idx]["primitives"][0]
    pos = _read_acc(prim["attributes"]["POSITION"])
    nrm = _read_acc(prim["attributes"]["NORMAL"])
    uv = _read_acc(prim["attributes"]["TEXCOORD_0"])
    idx = _read_acc(prim["indices"]).reshape(-1).astype(np.int64)
    # world transform + whole-piece normalisation (matches convert_retro_pieces)
    posw = (W @ np.c_[pos, np.ones(len(pos))].T).T[:, :3]
    nrmw = (R @ nrm.T).T
    nrmw /= (np.linalg.norm(nrmw, axis=1, keepdims=True) + 1e-12)
    bmin, bmax = posw.min(0), posw.max(0)
    center = (bmin + bmax) * 0.5
    ext = float((bmax - bmin).max()); scale = 2.0 / ext if ext > 0 else 1.0
    posn = (posw - center) * scale
    tris = idx.reshape(-1, 3)
    # connected components (union-find over shared triangle vertices)
    nv = len(posn); par = np.arange(nv)
    def find(x):
        r = x
        while par[r] != r: r = par[r]
        while par[x] != r: par[x], x = r, par[x]
        return r
    for a, b, c in tris:
        ra, rb, rc = find(a), find(b), find(c)
        if ra != rb: par[rb] = ra
        if ra != rc: par[rc] = ra
    roots = np.array([find(i) for i in range(nv)])
    P = Piece(); P.pos = posn; P.nrm = nrmw; P.uv = uv; P.tris = tris
    P.center = center; P.scale = scale
    P.components = []
    labels = np.unique(roots)
    # stable order: largest first
    labels = sorted(labels, key=lambda L: -int((roots == L).sum()))
    root2comp = {}
    for ci, L in enumerate(labels):
        vids = np.where(roots == L)[0]
        p = posn[vids]; mn, mx = p.min(0), p.max(0)
        C = Component()
        C.ids = vids; C.n = len(vids)
        C.centroid = p.mean(0); C.bbox_min = mn; C.bbox_max = mx; C.bbox_size = mx - mn
        C.radius = float(np.sqrt(p[:, 0] ** 2 + p[:, 2] ** 2).max())
        P.components.append(C); root2comp[L] = ci
    # triangle -> component index (via its first vertex's root)
    P.tri_comp = np.array([root2comp[find(t[0])] for t in tris])
    return P


def _write_uvmesh(path, P, tri_mask):
    tris = P.tris[tri_mask]
    flat = np.empty((len(tris) * 3, 8), dtype=np.float32)
    k = 0
    for t in tris:
        for vi in t:
            flat[k, 0:3] = P.pos[vi]; flat[k, 3:6] = P.nrm[vi]; flat[k, 6:8] = P.uv[vi]
            k += 1
    with open(path, "wb") as fh:
        fh.write(b"UVME"); fh.write(struct.pack("<I", len(flat))); fh.write(flat.tobytes())
    return len(flat) // 3


def write_split(piece, P, moving_comp_ids, out="models/retro"):
    """Write <piece>_base.uvmesh (all components NOT in moving_comp_ids) and
    <piece>_moving.uvmesh (the moving components). Returns (base_tris, moving_tris)."""
    moving = np.array([P.tri_comp[i] in set(moving_comp_ids) for i in range(len(P.tris))])
    os.makedirs(out, exist_ok=True)
    nb = _write_uvmesh(os.path.join(out, f"{piece}_base.uvmesh"), P, ~moving)
    nm = _write_uvmesh(os.path.join(out, f"{piece}_moving.uvmesh"), P, moving)
    return nb, nm
