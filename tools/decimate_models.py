"""
Decimate the high-res STL chess piece models into a low-poly set for the
WebAssembly build, where total asset size is the binding constraint
(GitHub Pages caps individual files at 100 MB).

Each piece is reduced to roughly TARGET_FACES triangles using Blender's
Decimate modifier (collapse mode). At 5,000 faces per piece the wood-grain
shader and shadow mapping completely hide the polygon count at gameplay
zoom levels.

Run via:
    blender --background --python tools/decimate_models.py
"""

import os
import sys

import bpy

REPO_ROOT  = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SRC_DIR    = os.path.join(REPO_ROOT, "models")
DST_DIR    = os.path.join(REPO_ROOT, "models-web")
TARGET_FACES = 5000

PIECES = ["King.stl", "Queen.stl", "Bishop.stl", "Knight.stl", "Rook.stl", "Pawn.stl"]


def reset_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def decimate_one(src_path: str, dst_path: str) -> None:
    print(f"[decimate] {os.path.basename(src_path)}")
    reset_scene()

    bpy.ops.import_mesh.stl(filepath=src_path)
    obj = bpy.context.selected_objects[0]
    bpy.context.view_layer.objects.active = obj

    src_faces = len(obj.data.polygons)
    if src_faces == 0:
        raise RuntimeError(f"{src_path} has 0 faces")

    ratio = min(1.0, TARGET_FACES / src_faces)

    mod = obj.modifiers.new(name="Decimate", type="DECIMATE")
    mod.decimate_type = "COLLAPSE"
    mod.ratio = ratio
    mod.use_collapse_triangulate = True

    bpy.ops.object.modifier_apply(modifier=mod.name)

    dst_faces = len(obj.data.polygons)
    print(f"          {src_faces} -> {dst_faces} triangles "
          f"({dst_faces / src_faces:.1%})")

    bpy.ops.export_mesh.stl(filepath=dst_path, use_selection=True, ascii=False)


def main() -> None:
    if not os.path.isdir(SRC_DIR):
        print(f"Source directory not found: {SRC_DIR}", file=sys.stderr)
        sys.exit(1)
    os.makedirs(DST_DIR, exist_ok=True)

    for filename in PIECES:
        src = os.path.join(SRC_DIR, filename)
        dst = os.path.join(DST_DIR, filename)
        if not os.path.isfile(src):
            print(f"Missing: {src}", file=sys.stderr)
            sys.exit(1)
        decimate_one(src, dst)

    print()
    print("Decimation complete. Output:")
    for filename in PIECES:
        path = os.path.join(DST_DIR, filename)
        size_kb = os.path.getsize(path) / 1024
        print(f"  {filename:12s} {size_kb:8.1f} KB")


if __name__ == "__main__":
    main()
