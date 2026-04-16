#!/usr/bin/env python3
"""
Recognize chess positions from photographs and emit FEN strings.

The tool handles two kinds of input transparently:

  * A single board photo: prints one FEN to stdout.
  * One or more "page" photos (a sheet with multiple board diagrams,
    e.g. homework worksheets): the tool asks the vision model to
    locate every board, extracts a FEN from each, and writes a
    `homework<N>.md` file (auto-numbered) in the current directory.
    No margins or grid dimensions need to be supplied — the board
    boxes are detected for every input.

Backend: Gemini (gemini-2.5-pro) via the Google GenAI API.

Usage:
    python tools/image_to_fen.py board.jpg
    python tools/image_to_fen.py page1.jpg page2.jpg        # → homework<N>.md
    python tools/image_to_fen.py --output hw2.md page1.jpg  # override path

Requires:
    pip install google-genai Pillow
    Set GOOGLE_API_KEY or GEMINI_API_KEY.
"""

import argparse
import io
import json
import os
import re
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print(
        "Error: Pillow is required.\n"
        "Install it with:  pip install Pillow",
        file=sys.stderr,
    )
    sys.exit(1)


# ── Image preprocessing ────────────────────────────────────────────

_MAX_IMAGE_BYTES = 4_500_000  # target < 5 MB API limit
_MAX_LONG_EDGE = 2048
_DEFAULT_MODEL = "gemini-2.5-pro"


def _prepare_image(raw_bytes: bytes) -> Image.Image:
    """Open, apply EXIF rotation, convert to RGB."""
    from PIL import ImageOps

    img = Image.open(io.BytesIO(raw_bytes))
    img = ImageOps.exif_transpose(img)  # apply phone orientation
    if img.mode in ("RGBA", "P"):
        img = img.convert("RGB")
    return img


def _resize_if_needed(img: Image.Image) -> Image.Image:
    """Downscale so the longest edge is ≤ _MAX_LONG_EDGE."""
    w, h = img.size
    if max(w, h) > _MAX_LONG_EDGE:
        scale = _MAX_LONG_EDGE / max(w, h)
        img = img.resize((int(w * scale), int(h * scale)), Image.LANCZOS)
    return img


def _to_jpeg_bytes(img: Image.Image) -> bytes:
    """Encode as JPEG, compressing until under the API size cap."""
    for quality in (85, 70, 55, 40, 25):
        buf = io.BytesIO()
        img.save(buf, format="JPEG", quality=quality)
        if buf.tell() <= _MAX_IMAGE_BYTES:
            return buf.getvalue()
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=15)
    return buf.getvalue()


# ── FEN extraction ──────────────────────────────────────────────────

_FEN_FULL = re.compile(
    r"[1-8KQRBNPkqrbnp]{1,8}"
    r"(?:/[1-8KQRBNPkqrbnp]{1,8}){7}"
    r"\s+[wb]"
    r"\s+(?:[KQkq]{1,4}|-)"
    r"\s+(?:[a-h][36]|-)"
    r"\s+\d+"
    r"\s+\d+"
)

_FEN_PLACEMENT = re.compile(
    r"[1-8KQRBNPkqrbnp]{1,8}"
    r"(?:/[1-8KQRBNPkqrbnp]{1,8}){7}"
)


def _extract_fen(raw_text: str) -> str:
    """Pull the last FEN string out of model output (which may contain prose)."""
    matches = _FEN_FULL.findall(raw_text)
    if matches:
        return matches[-1].strip()
    placements = _FEN_PLACEMENT.findall(raw_text)
    if placements:
        return placements[-1].strip() + " w - - 0 1"
    return raw_text.strip()


# ── Prompts ─────────────────────────────────────────────────────────

_ROTATION_PROMPT = """\
This is a photograph of a printed chess board. The photo may be
rotated or sideways. Look at the coordinate labels on the edges
of the board (a-h for files, 1-8 for ranks).

Tell me: how many degrees clockwise should I rotate this image so
that the board is in standard orientation (a1 in the bottom-left
corner, h8 in the top-right)?

Answer with ONLY a single integer: 0, 90, 180, or 270. Nothing else.
"""

_FEN_SYSTEM = """\
You are a chess position recognition expert. You will be shown a
photograph of a chess board in standard orientation (a1 bottom-left,
h8 top-right). Identify every piece on every square and return the
position in standard Forsyth-Edwards Notation (FEN).

Rules:
- Return ONLY the FEN string. No explanation, no reasoning.
- Piece letters: K Q R B N P (white), k q r b n p (black).
- First rank in FEN = rank 8 (top), last = rank 1 (bottom).
- Side to move: "w" unless clearly indicated otherwise.
- Castling/en-passant: "-" if unknown. Halfmove: 0, fullmove: 1.
"""

_FEN_USER = "Return the FEN for this chess position."

_DETECT_PROMPT = """\
Find every chess board diagram in this image. A chess board is a
square grid of alternating light/dark squares, usually with files
(a-h) and ranks (1-8) labelled along the edges, and may or may not
contain pieces.

For each board, return its tight bounding box (the outer border of
the 8x8 grid including rank/file labels). Output format: a JSON
array where each entry is [ymin, xmin, ymax, xmax] with coordinates
normalized to the 0-1000 range (ymin/ymax measured top-to-bottom,
xmin/xmax measured left-to-right).

Output ONLY the JSON array. No prose, no code fence, no keys.
Example for two boards: [[100, 50, 400, 350], [100, 600, 400, 900]]
"""


# ── Gemini vision call ──────────────────────────────────────────────

def _gemini_vision(image_bytes: bytes, system: str, user: str,
                   model: str) -> str:
    """Single Gemini vision call. Returns the text response."""
    from google import genai
    from google.genai import types

    api_key = os.environ.get("GOOGLE_API_KEY") or os.environ.get("GEMINI_API_KEY")
    if not api_key:
        raise RuntimeError(
            "Set GOOGLE_API_KEY or GEMINI_API_KEY environment variable."
        )

    client = genai.Client(api_key=api_key)

    # Gemini 2.5 Pro requires thinking mode (thinking_budget=0 is
    # rejected). Budget enough total tokens for both the internal
    # reasoning and the visible answer.
    config = types.GenerateContentConfig(
        system_instruction=system if system else None,
        max_output_tokens=8192,
    )

    resp = client.models.generate_content(
        model=model,
        contents=[
            types.Part.from_bytes(data=image_bytes, mime_type="image/jpeg"),
            user,
        ],
        config=config,
    )
    return resp.text.strip() if resp.text else ""


# ── Auto-rotation ───────────────────────────────────────────────────

def _detect_rotation(img: Image.Image, model: str) -> int:
    """Ask the model how many degrees CW to rotate. Returns 0/90/180/270."""
    jpeg = _to_jpeg_bytes(img)
    raw = _gemini_vision(jpeg, "", _ROTATION_PROMPT, model)
    for token in raw.split():
        token = token.strip("°.,;:!?")
        if token in ("0", "90", "180", "270"):
            return int(token)
    return 0


def _rotate_image(img: Image.Image, degrees_cw: int) -> Image.Image:
    """Rotate CW by the given degrees. PIL rotates CCW, so negate."""
    if degrees_cw == 0:
        return img
    return img.rotate(-degrees_cw, expand=True)


# ── Core pipeline ───────────────────────────────────────────────────

def _pil_to_fen(img: Image.Image, model: str, auto_rotate: bool) -> str:
    """PIL image → FEN."""
    img = _resize_if_needed(img)

    if auto_rotate:
        degrees = _detect_rotation(img, model)
        if degrees:
            img = _rotate_image(img, degrees)

    jpeg = _to_jpeg_bytes(img)
    raw = _gemini_vision(jpeg, _FEN_SYSTEM, _FEN_USER, model)
    return _extract_fen(raw)


def image_to_fen(
    image_path: str,
    *,
    model: str | None = None,
    auto_rotate: bool = True,
) -> str:
    """Read a chess position from an image file and return its FEN."""
    path = Path(image_path).expanduser().resolve()
    if not path.is_file():
        raise FileNotFoundError(f"Image not found: {path}")

    img = _prepare_image(path.read_bytes())
    return _pil_to_fen(img, model or _DEFAULT_MODEL, auto_rotate)


# ── Board detection (auto-slice) ────────────────────────────────────

def _detect_boards(
    img: Image.Image, model: str
) -> list[tuple[int, int, int, int]]:
    """Locate every chess board in the image. Returns pixel bounding
    boxes (x0, y0, x1, y1) with no ordering guarantee."""
    jpeg = _to_jpeg_bytes(img)
    raw = _gemini_vision(jpeg, "", _DETECT_PROMPT, model)

    match = re.search(r"\[\s*\[[\s\S]*?\]\s*\]", raw)
    if not match:
        raise RuntimeError(
            f"Could not parse board boxes from vision response: {raw[:200]!r}"
        )
    try:
        boxes_norm = json.loads(match.group(0))
    except json.JSONDecodeError as e:
        raise RuntimeError(
            f"Invalid JSON in vision response ({e}): {raw[:200]!r}"
        )

    w, h = img.size
    boxes: list[tuple[int, int, int, int]] = []
    for entry in boxes_norm:
        if not isinstance(entry, (list, tuple)) or len(entry) != 4:
            continue
        y0n, x0n, y1n, x1n = entry
        x0 = max(0, min(w, int(x0n * w / 1000)))
        x1 = max(0, min(w, int(x1n * w / 1000)))
        y0 = max(0, min(h, int(y0n * h / 1000)))
        y1 = max(0, min(h, int(y1n * h / 1000)))
        if x1 > x0 and y1 > y0:
            boxes.append((x0, y0, x1, y1))
    return boxes


def _infer_layout(
    boxes: list[tuple[int, int, int, int]],
) -> tuple[int, int, list[tuple[int, int, int, int]]]:
    """Cluster boxes into a rows × cols grid by y-/x-centres."""
    if not boxes:
        return 0, 0, []
    if len(boxes) == 1:
        return 1, 1, list(boxes)

    enriched = [
        ((x0 + x1) / 2.0, (y0 + y1) / 2.0, (y1 - y0), b)
        for b in boxes
        for (x0, y0, x1, y1) in [b]
    ]
    enriched.sort(key=lambda e: e[1])
    heights = sorted(e[2] for e in enriched)
    med_h = heights[len(heights) // 2]
    row_tol = med_h * 0.5

    rows_of: list[list[tuple[float, float, float, tuple]]] = [[enriched[0]]]
    for e in enriched[1:]:
        if e[1] - rows_of[-1][-1][1] > row_tol:
            rows_of.append([e])
        else:
            rows_of[-1].append(e)

    for row in rows_of:
        row.sort(key=lambda e: e[0])

    cols = max(len(r) for r in rows_of)
    rows = len(rows_of)
    sorted_boxes = [entry[3] for row in rows_of for entry in row]
    return rows, cols, sorted_boxes


_ROW_LABELS = {
    1: ["Center"],
    2: ["Top", "Bottom"],
    3: ["Top", "Middle", "Bottom"],
}
_COL_LABELS = {
    1: ["Center"],
    2: ["Left", "Right"],
    3: ["Left", "Middle", "Right"],
}


def _position_label(row: int, col: int, rows: int, cols: int) -> str:
    r_lbl = _ROW_LABELS.get(rows, [f"Row{i+1}" for i in range(rows)])[row]
    c_lbl = _COL_LABELS.get(cols, [f"Col{i+1}" for i in range(cols)])[col]
    if rows == 1 and cols == 1:
        return "Board"
    if rows == 1:
        return c_lbl
    if cols == 1:
        return r_lbl
    return f"{r_lbl} {c_lbl}"


def page_to_fens(
    image_path: str,
    *,
    model: str | None = None,
) -> list[tuple[str, str]]:
    """Auto-detect every board on a page, read each FEN, and return
    (positional_label, fen) pairs in row-major order."""
    path = Path(image_path).expanduser().resolve()
    if not path.is_file():
        raise FileNotFoundError(f"Image not found: {path}")

    mdl = model or _DEFAULT_MODEL
    img = _prepare_image(path.read_bytes())
    img = _resize_if_needed(img)

    boxes = _detect_boards(img, mdl)
    if not boxes:
        raise RuntimeError(f"No chess boards detected in {image_path}")

    rows, cols, sorted_boxes = _infer_layout(boxes)

    results: list[tuple[str, str]] = []
    for idx, (x0, y0, x1, y1) in enumerate(sorted_boxes):
        pad_x = int((x1 - x0) * 0.03)
        pad_y = int((y1 - y0) * 0.03)
        crop_box = (
            max(0, x0 - pad_x),
            max(0, y0 - pad_y),
            min(img.size[0], x1 + pad_x),
            min(img.size[1], y1 + pad_y),
        )
        tile = img.crop(crop_box)
        fen = _pil_to_fen(tile, mdl, auto_rotate=False)

        r = idx // cols if cols else 0
        c = idx % cols if cols else 0
        results.append((_position_label(r, c, rows, cols), fen))

    return results


# ── Homework markdown emission ──────────────────────────────────────

_HOMEWORK_HEADER = """\
# Challenge: Mate in 2 (White to move)
#
# Each non-comment, non-empty line below is a FEN position.
# The challenge is the same for every position: WHITE TO MOVE,
# find the move sequence that delivers checkmate in 2 moves.
#
# Format (parseable by the chess application):
#   type: mate_in_2
#   side: white
#   one FEN per line, comments start with #

type: mate_in_2
side: white
"""


def _next_homework_path() -> Path:
    """Pick the next unused homework<N>.md in ./challenges/ if that
    directory exists, otherwise in the current working directory."""
    base = Path("challenges") if Path("challenges").is_dir() else Path(".")
    n = 1
    while (base / f"homework{n}.md").exists():
        n += 1
    return base / f"homework{n}.md"


def write_homework_md(
    pages: list[list[tuple[str, str]]],
    output_path: Path,
) -> None:
    lines: list[str] = [_HOMEWORK_HEADER]
    for page_idx, page in enumerate(pages, start=1):
        lines.append(f"# Page {page_idx}\n")
        for label, fen in page:
            lines.append(f"# {label}\n{fen}\n")
    output_path.write_text("\n".join(lines))


# ── CLI ─────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Recognize chess positions from images using Gemini vision.",
    )
    parser.add_argument(
        "images", nargs="+", metavar="IMAGE",
        help="Path(s) to chess board or homework-page image(s).",
    )
    parser.add_argument(
        "--model", default=_DEFAULT_MODEL,
        help=f"Gemini model ID (default: {_DEFAULT_MODEL}).",
    )
    parser.add_argument(
        "--output", default=None, metavar="PATH",
        help="Where to write the generated homework<N>.md file. "
             "Defaults to the next unused ./challenges/homework<N>.md "
             "(or ./homework<N>.md if challenges/ is absent).",
    )
    args = parser.parse_args()

    pages: list[list[tuple[str, str]]] = []
    for image_path in args.images:
        try:
            entries = page_to_fens(image_path, model=args.model)
        except FileNotFoundError as e:
            print(f"Error: {e}", file=sys.stderr)
            sys.exit(1)
        except Exception as e:
            print(f"Error for {image_path}: {e}", file=sys.stderr)
            sys.exit(1)
        pages.append(entries)

    total_boards = sum(len(p) for p in pages)

    if len(args.images) == 1 and total_boards == 1:
        print(pages[0][0][1])
        return

    output_path = (
        Path(args.output).expanduser()
        if args.output
        else _next_homework_path()
    )
    write_homework_md(pages, output_path)
    print(f"Wrote {total_boards} FEN(s) across {len(pages)} page(s) "
          f"to {output_path}")


if __name__ == "__main__":
    main()
