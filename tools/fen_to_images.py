#!/usr/bin/env python3
"""
Render the chess positions in a homework<N>.md file as PNG images.

By default each page in the markdown becomes one composite PNG
laid out in a 3×2 grid (or whatever shape fits the number of boards
on that page), mirroring the original homework-page photo layout.
Use ``--per-board`` to emit one PNG per position instead.

Usage:
    python tools/fen_to_images.py challenges/homework1.md
    # → challenges/homework1_page1.png, challenges/homework1_page2.png

    python tools/fen_to_images.py --per-board challenges/homework1.md
    # → challenges/homework1_page1_top-left.png, ..., homework1_page2_bottom-right.png

    python tools/fen_to_images.py --output /tmp challenges/homework1.md

No vision APIs are called — rendering is pure PIL. The piece style
matches ``_render_fen_image`` in ``image_to_fen.py``: a white/black
disc per piece with the letter (K, Q, R, B, N, P) inside.
"""

import argparse
import math
import re
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw
except ImportError:
    print(
        "Error: Pillow is required.\n"
        "Install it with:  pip install Pillow",
        file=sys.stderr,
    )
    sys.exit(1)

# Share the renderer and parser with image_to_fen.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from image_to_fen import _render_fen_image, _load_font, parse_homework_md  # noqa: E402


def _slugify(label: str) -> str:
    """Convert 'Top Left' → 'top-left' for safe filenames."""
    s = re.sub(r"[^\w\s-]", "", label.lower())
    return re.sub(r"[\s_]+", "-", s).strip("-") or "board"


def _grid_shape(n: int) -> tuple[int, int]:
    """Pick rows × cols for a composite page of ``n`` boards.
    Prefers 3 columns when possible (matches typical homework pages)."""
    if n <= 1:
        return 1, 1
    if n <= 3:
        return 1, n
    if n == 4:
        return 2, 2
    if n <= 6:
        return 2, 3
    cols = 3
    rows = math.ceil(n / cols)
    return rows, cols


def _compose_page(
    page: list[tuple[str, str]],
    *,
    board_size: int = 384,
    title: str | None = None,
) -> Image.Image:
    """Render every (label, fen) board on a single page as a grid."""
    n = len(page)
    rows, cols = _grid_shape(n)

    label_font = _load_font(max(14, board_size // 20))
    title_font = _load_font(max(18, board_size // 14))

    label_h = max(24, board_size // 14)
    cell_w = board_size
    cell_h = board_size + label_h
    gap = max(20, board_size // 20)
    pad = gap

    header_h = 0
    if title:
        header_h = title_font.size + gap

    total_w = cols * cell_w + (cols + 1) * gap
    total_h = header_h + rows * cell_h + (rows + 1) * gap

    img = Image.new("RGB", (total_w, total_h), (255, 255, 255))
    d = ImageDraw.Draw(img)

    if title:
        tbbox = d.textbbox((0, 0), title, font=title_font)
        tw = tbbox[2] - tbbox[0]
        d.text(((total_w - tw) // 2, pad // 2), title,
               fill=(30, 30, 30), font=title_font)

    for idx, (label, fen) in enumerate(page):
        r = idx // cols
        c = idx % cols
        x = gap + c * (cell_w + gap)
        y = header_h + gap + r * (cell_h + gap)

        lbbox = d.textbbox((0, 0), label, font=label_font)
        lw = lbbox[2] - lbbox[0]
        d.text((x + (cell_w - lw) // 2, y), label,
               fill=(50, 50, 50), font=label_font)

        try:
            board_img = _render_fen_image(fen, size=board_size)
        except ValueError as e:
            # Draw a red error box in place of the board
            board_img = Image.new("RGB", (board_size, board_size), (240, 200, 200))
            ed = ImageDraw.Draw(board_img)
            ed.text((10, board_size // 2 - 20), f"Invalid FEN:\n{e}",
                    fill=(120, 0, 0), font=label_font)
        img.paste(board_img, (x, y + label_h))

    return img


def render_homework(
    md_path: Path,
    output_dir: Path,
    *,
    per_board: bool = False,
    board_size: int = 384,
) -> list[Path]:
    """Render every page in a homework<N>.md to PNG(s). Returns the
    list of output paths, in order."""
    pages = parse_homework_md(md_path)
    if not pages:
        raise RuntimeError(f"No FEN positions found in {md_path}")

    output_dir.mkdir(parents=True, exist_ok=True)
    stem = md_path.stem
    written: list[Path] = []

    for page_idx, page in enumerate(pages, start=1):
        if per_board:
            for label, fen in page:
                out = (
                    output_dir
                    / f"{stem}_page{page_idx}_{_slugify(label)}.png"
                )
                try:
                    _render_fen_image(fen, size=board_size).save(out)
                except ValueError as e:
                    print(f"Skipping {out.name}: {e}", file=sys.stderr)
                    continue
                written.append(out)
        else:
            out = output_dir / f"{stem}_page{page_idx}.png"
            composite = _compose_page(
                page, board_size=board_size, title=f"{stem} — Page {page_idx}"
            )
            composite.save(out)
            written.append(out)
    return written


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Render chess positions from a homework<N>.md as PNG images.",
    )
    parser.add_argument(
        "md", metavar="HOMEWORK.MD",
        help="Path to a homework<N>.md challenge file.",
    )
    parser.add_argument(
        "-o", "--output", default=None, metavar="DIR",
        help="Output directory (default: same folder as the input .md).",
    )
    parser.add_argument(
        "--per-board", action="store_true",
        help="Emit one PNG per position instead of a composite page image.",
    )
    parser.add_argument(
        "--board-size", type=int, default=384, metavar="PX",
        help="Pixel size of each rendered board (default: 384).",
    )
    args = parser.parse_args()

    md_path = Path(args.md).expanduser().resolve()
    if not md_path.is_file():
        print(f"Error: homework file not found: {md_path}", file=sys.stderr)
        sys.exit(1)

    out_dir = (
        Path(args.output).expanduser().resolve()
        if args.output
        else md_path.parent
    )

    try:
        written = render_homework(
            md_path, out_dir,
            per_board=args.per_board,
            board_size=args.board_size,
        )
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

    for p in written:
        print(p)


if __name__ == "__main__":
    main()
