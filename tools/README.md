# Tools

Utility scripts for the 3D Chess project.

## `decimate_models.py`

Decimates the high-resolution STL chess piece models in `models/` into
lower-poly versions for the WebAssembly build (`models-web/`). Uses
Blender's Decimate modifier (collapse mode).

**Dependencies:** [Blender](https://www.blender.org/) 4.x or newer
(the `blender` binary must be on `$PATH`).

**Usage:**

```bash
blender --background --python tools/decimate_models.py
```

The target triangle count per piece is configured via `TARGET_FACES`
at the top of the script (currently 80,000). Output goes to
`models-web/`.

---

## `pack_meshes.py`

Packs the decimated STL pieces in `models-web/` into a compact
indexed-mesh format (`.imsh` inside a gzip wrapper, kept under the
`.stl` extension so the C++ loader content-sniffs it). Binary STL
duplicates each vertex across ~6 triangles and ships a face normal
that the runtime recomputes anyway — packing drops the models from
~24 MB to ~4 MB (80% saved) before any HTTP-level compression.

**Usage:**

```bash
python3 tools/pack_meshes.py
```

Output goes to `models-web-packed/`, which the web Makefile preloads
into the emscripten VFS under `/models/`. The desktop build still
loads the high-resolution `models/` STLs unchanged.

---

## `homework_wizard.py`  ← recommended entry point

End-to-end GTK app: pick a folder of chess homework photos → the
tool runs Gemini FEN extraction on a background thread → shows each
photo side-by-side with a rendering of the extracted positions →
**Accept** writes `challenges/homework<N>.md`.

```bash
python tools/homework_wizard.py
```

Dependencies: `google-genai`, `Pillow`, and GTK 3 bindings.

```bash
pip install google-genai Pillow
sudo apt install python3-gi gir1.2-gtk-3.0   # if missing
export GOOGLE_API_KEY="AIza..."              # required before "Select directory…"
```

If the wizard's accept-or-retry flow isn't what you want, two
lower-level CLIs are still available:

* `image_to_fen.py` – recognition only (photos → FEN text).
* `fen_to_images.py` – rendering only (FEN markdown → PNGs).

---

## `image_to_fen.py`

Recognizes chess positions from photographs — either a single board
or a homework-style **page of boards** — and writes FENs to a
`homework<N>.md` file (when given multi-board input) or stdout.

### Features

- **Gemini vision** (`gemini-2.5-pro`) via the Google GenAI API.
- **Auto-board detection on pages**: when you pass a page photo
  with several board diagrams, the model returns bounding boxes
  for every board and the tool slices them automatically — no
  margins, grid dimensions, or manual crops needed.
- **Auto-rotation for single boards**: an extra API call detects
  90°/180°/270° rotation and corrects it with Pillow before
  reading, so phone photos taken sideways still work.
- **Optional self-verification** (`--verify`): renders the extracted
  FEN as a letter-based board and asks the model to compare it
  side-by-side with the original photo. Currently experimental and
  gated by strict guards (rejects invalid FENs or large diffs) so
  it never replaces a good answer with a worse one. Off by default.
- **Automatic image resizing**: Phone camera JPEGs (5-10 MB) are
  downscaled and re-compressed to fit within the 5 MB API limit.
- **FEN regex extraction**: If the model outputs reasoning prose
  instead of a bare FEN string, a regex pulls the FEN out.
- **`homework<N>.md` emission**: Multi-image or multi-board input
  produces a challenge-file with `# Page N` headers and positional
  labels (`Top Left`, `Bottom Right`, etc.) matching the format
  consumed by the 3d_chess challenge loader.

### Dependencies

```bash
pip install google-genai Pillow
```

### API Key

Get a key from [aistudio.google.com/apikey](https://aistudio.google.com/apikey),
then:

```bash
export GOOGLE_API_KEY="AIza..."   # or GEMINI_API_KEY
```

### Usage

```bash
# Single-board photo → FEN to stdout
python tools/image_to_fen.py ~/chess_homework/puzzle.jpg

# One or more full-page photos → auto-sliced, emits homework<N>.md
python tools/image_to_fen.py ~/chess_homework/full_pages/*.jpeg

# Override the output path
python tools/image_to_fen.py --output challenges/homework3.md page1.jpg page2.jpg

# Pick a different Gemini model (e.g. the cheaper flash tier)
python tools/image_to_fen.py --model gemini-2.5-flash page.jpg

# Opt into the experimental render-and-compare verification pass
python tools/image_to_fen.py --verify page.jpg
```

When invoked with multiple images (or a single multi-board page),
the tool writes to the next unused `./challenges/homework<N>.md`
(or `./homework<N>.md` if no `challenges/` directory is present).

### As a Library

```python
from image_to_fen import image_to_fen, page_to_fens, write_homework_md

# Single board
fen = image_to_fen("photo.jpg")
print(fen)   # "5r1k/6p1/8/2q3N1/8/8/5PPP/3Q2K1 w - - 0 1"

# Full page: auto-detects and returns [(label, fen), ...]
page = page_to_fens("page.jpeg")
for label, fen in page:
    print(label, fen)

# Write a homework markdown file yourself
write_homework_md([page], Path("challenges/homework2.md"))
```

### Accuracy Notes

- Works best on **clear, well-lit** photos with **legible piece
  icons**. Page photos with small board diagrams are the hardest
  case — expect the model to misplace 1–2 pieces on ~10% of boards.
- Auto-rotation handles 90°/180°/270° single-board photos. Pages
  should be photographed upright.
- Handwritten answers or annotations on the page are ignored, but
  if a handwritten mark overlaps a board square the model may
  mistake it for a piece.

---

## `fen_to_images.py`

Reverse of `image_to_fen.py`: takes a `homework<N>.md` challenge
file and renders every FEN as a PNG. Pure PIL — no vision APIs
required.

### Usage

```bash
# One composite PNG per page (3×2 grid mirroring the original layout)
python tools/fen_to_images.py challenges/homework1.md
# → challenges/homework1_page1.png, challenges/homework1_page2.png

# One PNG per position instead
python tools/fen_to_images.py --per-board challenges/homework1.md
# → challenges/homework1_page1_top-left.png, ...

# Bigger boards, different output dir
python tools/fen_to_images.py --board-size 600 -o /tmp challenges/homework1.md
```

Each piece is rendered as a coloured disc with its letter inside:
white pieces are white discs with black letters, black pieces are
black discs with white letters. Files and ranks are labelled around
the edge.
