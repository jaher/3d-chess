#!/usr/bin/env python3
"""Convert the RetroPC chess GLB into the game's textured-mesh (.uvmesh) format.

Source: ~/retro_chess/retropc_chess.glb (CC-BY-4.0, dark_igorek, Sketchfab).
Pieces map by MATERIAL name (king_w_mat, …, pawb_w_mat — note the 'pawb' typo).
For each piece TYPE we take the white-material geometry (black is the same shape,
different texture), bake its world transform, re-centre at the bbox centre and
scale to a unit sphere (2 / max_extent) — matching stl_model.cpp's
build_vertex_buffer — so the existing piece_model_matrix places it on the board
unchanged (drawn with rot_z_to_y = 0, since the GLB is already Y-up). The board
is written un-normalised (scaled in-engine) and ships both colour textures.

`.uvmesh`: magic "UVME" + uint32 vertex_count + 8 float32/vertex
           (px,py,pz, nx,ny,nz, u,v), non-indexed triangle soup.
"""
import os, struct, sys
import numpy as np
import trimesh
from pygltflib import GLTF2

GLB = os.path.expanduser("~/retro_chess/retropc_chess.glb")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "models", "retro")
TEX = os.path.join(OUT, "tex")

# game type -> (white material, black material)
PIECES = {
    "King":   ("king_w_mat",   "king_b_mat"),
    "Queen":  ("queen_w_mat",  "queen_b_mat"),
    "Bishop": ("bishop_w_mat", "bishop_b_mat"),
    "Knight": ("knight_w_mat", "knight_b_mat"),
    "Rook":   ("rook_w_mat",   "rook_b_mat"),
    "Pawn":   ("pawb_w_mat",   "pawb_b_mat"),
}
BOARD_MAT = "board_mat"


def write_uvmesh(path, m, normalize=True):
    v = np.asarray(m.vertices, dtype=np.float64)
    if normalize:
        bmin, bmax = v.min(0), v.max(0)
        center = (bmin + bmax) * 0.5
        max_extent = float((bmax - bmin).max())
        scale = 2.0 / max_extent if max_extent > 0 else 1.0
        v = (v - center) * scale
    vn = np.asarray(m.vertex_normals, dtype=np.float64)
    uv = m.visual.uv
    uv = np.zeros((len(v), 2)) if uv is None else np.asarray(uv, dtype=np.float64)
    faces = np.asarray(m.faces)
    flat = np.empty((len(faces) * 3, 8), dtype=np.float32)
    k = 0
    for f in faces:
        for idx in f:
            flat[k, 0:3] = v[idx]; flat[k, 3:6] = vn[idx]; flat[k, 6:8] = uv[idx]
            k += 1
    with open(path, "wb") as fh:
        fh.write(b"UVME")
        fh.write(struct.pack("<I", len(flat)))
        fh.write(flat.tobytes())
    return len(flat)


def main():
    os.makedirs(TEX, exist_ok=True)
    scene = trimesh.load(GLB, process=False)
    if not isinstance(scene, trimesh.Scene):
        sys.exit("expected a glTF scene")

    # material name -> representative world-baked Trimesh
    mat_geom = {}
    for node in scene.graph.nodes_geometry:
        T, gname = scene.graph.get(node)
        geom = scene.geometry[gname]
        mname = getattr(getattr(geom.visual, "material", None), "name", None)
        if mname and mname not in mat_geom:
            m = geom.copy(); m.apply_transform(T); mat_geom[mname] = m

    # material name -> baseColor texture bytes (via pygltflib)
    g = GLTF2().load(GLB); blob = g.binary_blob()
    mat_img = {}
    for m in g.materials:
        pbr = m.pbrMetallicRoughness
        if pbr and pbr.baseColorTexture:
            img = g.images[g.textures[pbr.baseColorTexture.index].source]
            bv = g.bufferViews[img.bufferView]; off = bv.byteOffset or 0
            ext = ".jpg" if "jpeg" in (img.mimeType or "") else ".png"
            mat_img[m.name] = (blob[off:off + bv.byteLength], ext)

    def save_tex(mat, base):
        if mat in mat_img:
            data, ext = mat_img[mat]
            open(os.path.join(TEX, base + ext), "wb").write(data)
            return base + ext
        return None

    print("=== pieces ===")
    for typ, (wmat, bmat) in PIECES.items():
        if wmat not in mat_geom:
            print(f"  !! {typ}: material {wmat} not found"); continue
        nv = write_uvmesh(os.path.join(OUT, typ + ".uvmesh"), mat_geom[wmat], True)
        tw = save_tex(wmat, f"{typ.lower()}_w_baseColor")
        tb = save_tex(bmat, f"{typ.lower()}_b_baseColor")
        print(f"  {typ:7s} {nv//3:6d} tris  white={tw} black={tb}")

    print("=== board ===")
    bm = mat_geom[BOARD_MAT]
    nb = write_uvmesh(os.path.join(OUT, "board.uvmesh"), bm, normalize=False)
    save_tex(BOARD_MAT, "board_baseColor")
    bmin, bmax = bm.vertices.min(0), bm.vertices.max(0)
    print(f"  board {nb//3} tris  bbox={bmin.round(3)}..{bmax.round(3)} "
          f"size={(bmax-bmin).round(3)}")

    with open(os.path.join(OUT, "CREDITS.txt"), "w") as f:
        f.write("RetroPC Chess by dark_igorek "
                "(https://sketchfab.com/dark_igorek)\n"
                "Source: https://sketchfab.com/3d-models/"
                "retropc-chess-31e657b251b546e69e6ae312fc0bf66a\n"
                "License: CC-BY-4.0\n")
    print("done. assets in", OUT)


if __name__ == "__main__":
    main()
