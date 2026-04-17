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
from concurrent.futures import ThreadPoolExecutor
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

_CORNERS_PROMPT = """\
Look at this image of a chess board. Return the pixel coordinates
of the 4 OUTER corners of the 8x8 playing grid. These are the
corners of the outer border around the 64 squares — EXCLUDE any
rank/file labels printed outside the grid.

Return the 4 corners in this exact order:
  1. Top-left     (the top-left corner of square a8)
  2. Top-right    (the top-right corner of square h8)
  3. Bottom-right (the bottom-right corner of square h1)
  4. Bottom-left  (the bottom-left corner of square a1)

Output format: a JSON array of 4 [y, x] pairs, normalized to the
0-1000 range (y top-to-bottom, x left-to-right). Output ONLY the
JSON array. Example: [[80, 100], [80, 900], [920, 900], [920, 100]]
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


# ── FEN → image rendering (for self-verification) ──────────────────

_FONT_CANDIDATES = (
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
)


def _load_font(size: int):
    from PIL import ImageFont
    for path in _FONT_CANDIDATES:
        try:
            return ImageFont.truetype(path, size)
        except Exception:
            continue
    return ImageFont.load_default()


def _render_fen_image(fen: str, size: int = 512) -> Image.Image:
    """Render the placement portion of a FEN as an 8x8 board. Each
    piece is a coloured disc with its letter inside: white pieces are
    white discs with black uppercase letters, black pieces are black
    discs with white uppercase letters. Files (a-h) and ranks (1-8)
    are labelled around the edge. Pure PIL — no chess library needed."""
    from PIL import ImageDraw

    placement = fen.split()[0]
    ranks = placement.split("/")
    if len(ranks) != 8:
        raise ValueError(f"FEN placement has {len(ranks)} ranks, expected 8")

    margin = size // 20
    board_size = size - 2 * margin
    cell = board_size // 8
    board_size = cell * 8
    img = Image.new("RGB", (size, size), (255, 255, 255))
    d = ImageDraw.Draw(img)
    font = _load_font(int(cell * 0.6))
    label_font = _load_font(max(10, int(cell * 0.3)))

    LIGHT = (235, 215, 180)
    DARK = (165, 120, 80)

    ox, oy = margin, margin  # board origin

    # Draw file labels (a-h) and rank labels (8-1 top to bottom)
    for c in range(8):
        file_letter = "abcdefgh"[c]
        fbbox = d.textbbox((0, 0), file_letter, font=label_font)
        fw = fbbox[2] - fbbox[0]
        d.text(
            (ox + c * cell + (cell - fw) // 2, oy + board_size + 2),
            file_letter, fill=(80, 80, 80), font=label_font,
        )
    for r in range(8):
        rank_digit = str(8 - r)
        rbbox = d.textbbox((0, 0), rank_digit, font=label_font)
        rh = rbbox[3] - rbbox[1]
        d.text(
            (2, oy + r * cell + (cell - rh) // 2 - rbbox[1]),
            rank_digit, fill=(80, 80, 80), font=label_font,
        )

    for r, rank_str in enumerate(ranks):  # r=0 → rank 8 (top)
        c = 0
        for ch in rank_str:
            if ch.isdigit():
                for _ in range(int(ch)):
                    fill = LIGHT if (r + c) % 2 == 0 else DARK
                    d.rectangle(
                        [ox + c * cell, oy + r * cell,
                         ox + (c + 1) * cell, oy + (r + 1) * cell],
                        fill=fill,
                    )
                    c += 1
                continue
            fill = LIGHT if (r + c) % 2 == 0 else DARK
            d.rectangle(
                [ox + c * cell, oy + r * cell,
                 ox + (c + 1) * cell, oy + (r + 1) * cell],
                fill=fill,
            )
            # Piece disc: white for uppercase (white pieces), dark grey
            # for lowercase (black pieces). Letter in contrasting colour.
            radius = int(cell * 0.40)
            cx = ox + c * cell + cell // 2
            cy = oy + r * cell + cell // 2
            is_white_piece = ch.isupper()
            disc_fill = (255, 255, 255) if is_white_piece else (30, 30, 30)
            disc_outline = (0, 0, 0) if is_white_piece else (255, 255, 255)
            d.ellipse(
                [cx - radius, cy - radius, cx + radius, cy + radius],
                fill=disc_fill, outline=disc_outline, width=2,
            )

            letter = ch.upper()
            text_fill = (0, 0, 0) if is_white_piece else (255, 255, 255)
            bbox = d.textbbox((0, 0), letter, font=font)
            tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
            tx = cx - tw // 2 - bbox[0]
            ty = cy - th // 2 - bbox[1]
            d.text((tx, ty), letter, fill=text_fill, font=font)
            c += 1
    return img


def _compose_verify_image(
    original: Image.Image, rendered: Image.Image
) -> Image.Image:
    """Stack ORIGINAL PHOTO beside RENDERED FEN with header labels."""
    from PIL import ImageDraw

    target_h = 512
    ow, oh = original.size
    left = original.resize(
        (max(1, int(ow * target_h / oh)), target_h), Image.LANCZOS
    )
    right = rendered.resize((target_h, target_h), Image.LANCZOS)

    gap = 20
    label_h = 32
    total_w = left.size[0] + gap + right.size[0]
    total_h = target_h + label_h
    out = Image.new("RGB", (total_w, total_h), (255, 255, 255))
    out.paste(left, (0, label_h))
    out.paste(right, (left.size[0] + gap, label_h))

    d = ImageDraw.Draw(out)
    font = _load_font(18)
    d.text((10, 6), "ORIGINAL PHOTO", fill=(0, 0, 0), font=font)
    d.text(
        (left.size[0] + gap + 10, 6),
        "EXTRACTED FEN (letters)",
        fill=(0, 0, 0),
        font=font,
    )
    return out


_VERIFY_SYSTEM = """\
You are a chess position verifier. You will see one image containing
two boards side by side: ORIGINAL PHOTO on the left, and a
disc-and-letter rendering of an already-extracted FEN on the right.
White pieces appear as white discs with black letters; black pieces
as black discs with white letters (K Q R B N P).

Your job is STRICTLY to double-check the right board against the
left, not to re-read from scratch. Assume the right board is mostly
correct.

For every square:
  1. Look at the left (photo) square.
  2. Look at the right (rendered) square.
  3. If they match, leave it alone.
  4. Only change a square when you are confident it is a mismatch —
     e.g. the photo clearly shows a piece the rendering is missing,
     or the rendering shows a piece the photo clearly doesn't have,
     or the piece/colour is obviously different.

Output rules:
- Return ONLY the FEN string for the LEFT (original) board.
- If you are NOT confident about any corrections, return the SAME
  FEN as the rendering. DO NOT invent changes.
- Every rank must have exactly 8 squares of content (digits + piece
  letters summing to 8). Invalid FENs will be rejected.
- Format: <placement> w - - 0 1
"""

_VERIFY_USER = (
    "Compare the right rendering to the left photo and return the "
    "corrected FEN. If the rendering is already correct, return the "
    "same FEN verbatim."
)


def _is_valid_fen_placement(fen: str) -> bool:
    """Validate just the placement field: 8 ranks, each summing to 8."""
    try:
        placement = fen.split()[0]
    except IndexError:
        return False
    ranks = placement.split("/")
    if len(ranks) != 8:
        return False
    for rank in ranks:
        count = 0
        for ch in rank:
            if ch.isdigit():
                count += int(ch)
            elif ch in "KQRBNPkqrbnp":
                count += 1
            else:
                return False
        if count != 8:
            return False
    return True


def _placement_diff_squares(a: str, b: str) -> int:
    """Count squares that differ between two valid FEN placements."""
    def expand(fen: str) -> list[str]:
        out: list[str] = []
        for rank in fen.split()[0].split("/"):
            for ch in rank:
                if ch.isdigit():
                    out.extend(["."] * int(ch))
                else:
                    out.append(ch)
        return out

    ea, eb = expand(a), expand(b)
    if len(ea) != 64 or len(eb) != 64:
        return 64
    return sum(1 for x, y in zip(ea, eb) if x != y)


def _verify_fen(
    original_tile: Image.Image,
    fen: str,
    model: str,
    max_rounds: int = 1,
    max_diff_squares: int = 4,
) -> str:
    """Render FEN → image, compare side-by-side with original, ask
    the model for a corrected FEN.

    Guards:
      * If the returned FEN is syntactically invalid (not 8 ranks of
        8 squares each), keep the original.
      * If the correction diverges by more than ``max_diff_squares``
        cells, treat it as hallucination and keep the original.

    Returns the (possibly unchanged) verified FEN."""
    current = fen
    for _ in range(max_rounds):
        try:
            rendered = _render_fen_image(current)
            combined = _compose_verify_image(original_tile, rendered)
            jpeg = _to_jpeg_bytes(combined)
            raw = _gemini_vision(jpeg, _VERIFY_SYSTEM, _VERIFY_USER, model)
            corrected = _extract_fen(raw)
        except Exception:
            break
        if not corrected or corrected == current:
            break
        if not _is_valid_fen_placement(corrected):
            break
        if _placement_diff_squares(current, corrected) > max_diff_squares:
            break
        current = corrected
    return current


# ── Grid corner detection + perspective rectification ─────────────

def _detect_board_corners(
    img: Image.Image, model: str
) -> list[tuple[int, int]]:
    """Ask Gemini for the 4 outer corners of the 8x8 grid.
    Returns 4 (x, y) pixel coords in [TL, TR, BR, BL] order. Raises
    on parse failure so callers can fall back to an un-rectified
    crop."""
    jpeg = _to_jpeg_bytes(img)
    raw = _gemini_vision(jpeg, "", _CORNERS_PROMPT, model)
    match = re.search(r"\[\s*\[[\s\S]*?\]\s*\]", raw)
    if not match:
        raise RuntimeError(f"Could not parse corners: {raw[:200]!r}")
    data = json.loads(match.group(0))
    if not isinstance(data, list) or len(data) != 4:
        raise RuntimeError(f"Expected 4 corners, got {data!r}")
    w, h = img.size
    corners: list[tuple[int, int]] = []
    for entry in data:
        if not isinstance(entry, (list, tuple)) or len(entry) != 2:
            raise RuntimeError(f"Bad corner entry: {entry!r}")
        y_n, x_n = entry
        x = max(0, min(w, int(x_n * w / 1000)))
        y = max(0, min(h, int(y_n * h / 1000)))
        corners.append((x, y))
    return corners


def _solve_linear(A: list[list[float]], B: list[float]) -> list[float]:
    """Gaussian elimination solver for small N×N systems."""
    n = len(A)
    M = [row[:] + [b] for row, b in zip(A, B)]
    for i in range(n):
        pivot_row = max(range(i, n), key=lambda r: abs(M[r][i]))
        M[i], M[pivot_row] = M[pivot_row], M[i]
        pivot = M[i][i]
        if abs(pivot) < 1e-12:
            raise ValueError("Singular matrix in perspective solve")
        for j in range(i, n + 1):
            M[i][j] /= pivot
        for r in range(n):
            if r != i and M[r][i]:
                factor = M[r][i]
                for j in range(i, n + 1):
                    M[r][j] -= factor * M[i][j]
    return [M[i][n] for i in range(n)]


def _perspective_coeffs(
    src_corners: list[tuple[int, int]], dst_size: int
) -> list[float]:
    """Compute the 8 coefficients ``Image.transform(PERSPECTIVE)``
    wants. PIL's perspective transform maps DESTINATION pixels back
    to SOURCE, so we solve for the inverse homography: given target
    corners [(0,0),(s,0),(s,s),(0,s)], find (a..h) such that

        x_src = (a*X + b*Y + c) / (g*X + h*Y + 1)
        y_src = (d*X + e*Y + f) / (g*X + h*Y + 1)

    matches each source corner."""
    dst = [(0, 0), (dst_size, 0), (dst_size, dst_size), (0, dst_size)]
    A: list[list[float]] = []
    B: list[float] = []
    for (x, y), (X, Y) in zip(src_corners, dst):
        A.append([X, Y, 1, 0, 0, 0, -x * X, -x * Y])
        A.append([0, 0, 0, X, Y, 1, -y * X, -y * Y])
        B.append(float(x))
        B.append(float(y))
    return _solve_linear(A, B)


def _rectify_board(
    img: Image.Image,
    corners: list[tuple[int, int]],
    size: int = 1024,
) -> Image.Image:
    """Warp the 4-corner quad to a ``size × size`` square via PIL
    perspective transform."""
    coeffs = _perspective_coeffs(corners, size)
    return img.transform(
        (size, size),
        Image.PERSPECTIVE,
        coeffs,
        Image.BICUBIC,
    )


# ── Core pipeline ───────────────────────────────────────────────────

def _pil_to_fen(
    img: Image.Image,
    model: str,
    auto_rotate: bool,
    verify: bool = False,
    rectify: bool = True,
) -> str:
    """PIL image → FEN.

    When ``rectify`` is True (the default), the function first asks
    the model for the 4 outer grid corners of the board in the crop
    and applies a perspective transform so the 8×8 grid fills a
    1024-pixel square exactly. This eliminates off-by-one rank/file
    mistakes caused by loose bounding boxes or perspective distortion
    in the page photo.

    When ``verify`` is True, run one render-and-compare pass to catch
    common mistakes (opt-in; currently gated behind strict guards)."""
    img = _resize_if_needed(img)

    if auto_rotate:
        degrees = _detect_rotation(img, model)
        if degrees:
            img = _rotate_image(img, degrees)

    if rectify:
        try:
            corners = _detect_board_corners(img, model)
            img = _rectify_board(img, corners, size=1024)
        except Exception:
            pass  # fall through with the un-rectified crop

    jpeg = _to_jpeg_bytes(img)
    raw = _gemini_vision(jpeg, _FEN_SYSTEM, _FEN_USER, model)
    fen = _extract_fen(raw)

    if verify:
        fen = _verify_fen(img, fen, model)
    return fen


def image_to_fen(
    image_path: str,
    *,
    model: str | None = None,
    auto_rotate: bool = True,
    verify: bool = False,
) -> str:
    """Read a chess position from an image file and return its FEN."""
    path = Path(image_path).expanduser().resolve()
    if not path.is_file():
        raise FileNotFoundError(f"Image not found: {path}")

    img = _prepare_image(path.read_bytes())
    return _pil_to_fen(img, model or _DEFAULT_MODEL, auto_rotate, verify)


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
    verify: bool = False,
    max_workers: int = 3,
) -> list[tuple[str, str]]:
    """Auto-detect every board on a page, read each FEN, and return
    (positional_label, fen) pairs in row-major order.

    ``max_workers`` controls how many FEN extraction calls run in
    parallel (one Gemini request per board). Detection itself is a
    single serial call."""
    path = Path(image_path).expanduser().resolve()
    if not path.is_file():
        raise FileNotFoundError(f"Image not found: {path}")

    mdl = model or _DEFAULT_MODEL

    # Keep the full-resolution image around for cropping. Detection
    # runs on a smaller copy so the request stays under the API size
    # limit; the returned boxes are rescaled back to the original.
    original = _prepare_image(path.read_bytes())
    detect_img = _resize_if_needed(original)
    sx = original.size[0] / detect_img.size[0]
    sy = original.size[1] / detect_img.size[1]

    boxes_small = _detect_boards(detect_img, mdl)
    if not boxes_small:
        raise RuntimeError(f"No chess boards detected in {image_path}")

    # Rescale each detection box to original-resolution pixels.
    boxes = [
        (
            max(0, min(original.size[0], int(x0 * sx))),
            max(0, min(original.size[1], int(y0 * sy))),
            max(0, min(original.size[0], int(x1 * sx))),
            max(0, min(original.size[1], int(y1 * sy))),
        )
        for (x0, y0, x1, y1) in boxes_small
    ]

    rows, cols, sorted_boxes = _infer_layout(boxes)

    # Pre-crop every tile, then extract FENs in parallel.
    tiles: list[Image.Image] = []
    for (x0, y0, x1, y1) in sorted_boxes:
        pad_x = int((x1 - x0) * 0.03)
        pad_y = int((y1 - y0) * 0.03)
        crop_box = (
            max(0, x0 - pad_x),
            max(0, y0 - pad_y),
            min(original.size[0], x1 + pad_x),
            min(original.size[1], y1 + pad_y),
        )
        tiles.append(original.crop(crop_box))

    def _extract(tile: Image.Image) -> str:
        return _pil_to_fen(tile, mdl, auto_rotate=False, verify=verify)

    with ThreadPoolExecutor(
        max_workers=max(1, min(max_workers, len(tiles)))
    ) as pool:
        fens = list(pool.map(_extract, tiles))

    results: list[tuple[str, str]] = []
    for idx, fen in enumerate(fens):
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


def parse_homework_md(path: Path) -> list[list[tuple[str, str]]]:
    """Parse a homework<N>.md file and return a list of pages, where
    each page is a list of (label, fen) pairs.

    The format is:
      type: / side: header lines (ignored)
      # Page <N>      -> starts a new page
      # <label>       -> label for the following FEN (e.g. "Top Left")
      <fen line>      -> bare FEN placement + metadata

    Labels without a preceding ``# Page`` end up on page 1."""
    text = Path(path).expanduser().read_text()
    pages: list[list[tuple[str, str]]] = []
    current_page: list[tuple[str, str]] = []
    pending_label: str | None = None

    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("#"):
            comment = line.lstrip("#").strip()
            if not comment:
                continue
            page_match = re.fullmatch(r"Page\s+\d+", comment, re.IGNORECASE)
            if page_match:
                if current_page:
                    pages.append(current_page)
                    current_page = []
                pending_label = None
                continue
            pending_label = comment
            continue
        if ":" in line and _FEN_FULL.search(line) is None:
            # metadata line like "type: mate_in_2" — skip
            continue
        fen_match = _FEN_FULL.search(line) or _FEN_PLACEMENT.search(line)
        if not fen_match:
            continue
        fen = fen_match.group(0)
        if _FEN_FULL.fullmatch(fen) is None and not fen.endswith(" 1"):
            fen = fen.strip() + " w - - 0 1"
        label = pending_label or f"Board {len(current_page) + 1}"
        current_page.append((label, fen))
        pending_label = None

    if current_page:
        pages.append(current_page)
    return pages


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
    parser.add_argument(
        "--verify", action="store_true",
        help="Enable the experimental render-and-compare verification "
             "pass (one extra API call per board; currently gated "
             "behind strict guards to avoid regressions).",
    )
    args = parser.parse_args()

    pages: list[list[tuple[str, str]]] = []
    for image_path in args.images:
        try:
            entries = page_to_fens(
                image_path, model=args.model, verify=args.verify,
            )
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
