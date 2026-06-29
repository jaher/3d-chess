#!/usr/bin/env python3
"""Extract the 16 retro pawn keycaps (8 per colour) from retropc_chess.glb.

Each retro pawn is a separate keyboard keycap mesh that shares the per-colour
pawn baseColor atlas (pawb_w_mat / pawb_b_mat) but UV-maps its top face to a
different letter cell — so every pawn shows a distinct key. The main piece
converter (convert_retro_pieces.py) collapses all pawns to one mesh; this tool
writes the per-file keycaps the engine needs: models/retro/Pawn_{w,b}{0..7}.uvmesh,
ordered left-to-right by the keycap's original board Z so file f → keycap f.

Dependency-free: parses the GLB JSON+BIN chunks directly (no trimesh/pygltflib).
Validated byte-identical to the trimesh path on the King mesh.
"""
import os
HERE = os.path.dirname(os.path.abspath(__file__))
exec(open(os.path.join(HERE, "convert_retro_pawns_core.py")).read().split("if __name__")[0])
import numpy as np

OUT = os.path.join(os.path.dirname(HERE), "models", "retro")

def main():
    white = [(19 + k, f"Object_{42 + 2*k}") for k in range(8)]
    black = [(11 + k, f"Object_{26 + 2*k}") for k in range(8)]
    def zsort(pairs):
        keyed = [(world_matrix(name2idx[n])[2, 3], mi, n) for mi, n in pairs]
        return [(mi, n) for _, mi, n in sorted(keyed)]
    # Plain extraction — the keycap body is drawn untextured and the letter
    # is a separate flat decal, so the model's baked slope-legend isn't used.
    for prefix, pairs in (("Pawn_w", zsort(white)), ("Pawn_b", zsort(black))):
        for f, (mi, n) in enumerate(pairs):
            write_uvmesh(os.path.join(OUT, f"{prefix}{f}.uvmesh"), extract(mi, n))
        print(prefix, "->", [n for _, n in pairs])
    print("wrote 16 keycaps to", OUT)

if __name__ == "__main__":
    main()
