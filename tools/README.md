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
