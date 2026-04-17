#!/usr/bin/env python3
"""
Side-by-side GTK viewer for a homework<N>.md and its source photos.

Given a homework markdown file and the original page photo(s) it
was extracted from, this tool renders each page's FEN set with
``fen_to_images._compose_page`` and displays it next to the photo,
one page at a time. Use Left/Right arrows or the Prev/Next buttons
to cycle pages. Useful for eyeballing how well `image_to_fen`
captured each position.

Usage:
    python tools/homework_viewer.py challenges/homework1.md \\
        ~/chess_homework/full_pages/page1.jpeg \\
        ~/chess_homework/full_pages/page2.jpeg

The N-th photo is paired with the N-th ``# Page N`` section in the
markdown. Extra photos or pages are tolerated (the longer list is
truncated to the shorter one).
"""

import argparse
import io
import sys
from pathlib import Path

try:
    import gi
    gi.require_version("Gtk", "3.0")
    from gi.repository import Gtk, Gdk, GdkPixbuf, GLib  # noqa: F401
except ImportError:
    print(
        "Error: PyGObject (gi) with GTK 3 is required.\n"
        "  Debian/Ubuntu: sudo apt install python3-gi gir1.2-gtk-3.0",
        file=sys.stderr,
    )
    sys.exit(1)

try:
    from PIL import Image, ImageOps
except ImportError:
    print("Error: Pillow is required.  pip install Pillow", file=sys.stderr)
    sys.exit(1)

# Pull the parser + renderer from the sibling scripts.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from image_to_fen import parse_homework_md  # noqa: E402
from fen_to_images import _compose_page  # noqa: E402


def _pil_to_pixbuf(img: Image.Image) -> GdkPixbuf.Pixbuf:
    """Encode PIL image as PNG bytes and load via GdkPixbuf."""
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    loader = GdkPixbuf.PixbufLoader.new_with_type("png")
    loader.write(buf.getvalue())
    loader.close()
    return loader.get_pixbuf()


class HomeworkViewer(Gtk.Window):
    def __init__(self, pairs: list[tuple[str, Image.Image, Image.Image]]):
        super().__init__(title="Homework Viewer")
        self.pairs = pairs
        self.page_idx = 0
        self._last_alloc = (0, 0)
        self.set_default_size(1600, 900)

        vbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        vbox.set_margin_top(8)
        vbox.set_margin_bottom(8)
        vbox.set_margin_start(8)
        vbox.set_margin_end(8)
        self.add(vbox)

        self.header = Gtk.Label()
        self.header.set_xalign(0.5)
        vbox.pack_start(self.header, False, False, 0)

        hbox = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        vbox.pack_start(hbox, True, True, 0)

        self.orig_image = Gtk.Image()
        self.rend_image = Gtk.Image()

        left_frame = Gtk.Frame(label="Original photo")
        left_frame.add(self.orig_image)
        right_frame = Gtk.Frame(label="Rendered from FEN")
        right_frame.add(self.rend_image)

        hbox.pack_start(left_frame, True, True, 0)
        hbox.pack_start(right_frame, True, True, 0)

        nav = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        vbox.pack_start(nav, False, False, 0)

        prev_btn = Gtk.Button(label="◀ Prev")
        prev_btn.connect("clicked", lambda *_: self._nav(-1))
        next_btn = Gtk.Button(label="Next ▶")
        next_btn.connect("clicked", lambda *_: self._nav(1))
        nav.pack_start(prev_btn, True, True, 0)
        nav.pack_start(next_btn, True, True, 0)

        self.connect("key-press-event", self._on_key)
        self.connect("size-allocate", self._on_resize)

        self._refresh()

    def _on_key(self, _widget, event) -> bool:
        if event.keyval in (Gdk.KEY_Left, Gdk.KEY_Page_Up):
            self._nav(-1)
            return True
        if event.keyval in (Gdk.KEY_Right, Gdk.KEY_Page_Down, Gdk.KEY_space):
            self._nav(1)
            return True
        if event.keyval in (Gdk.KEY_q, Gdk.KEY_Escape):
            Gtk.main_quit()
            return True
        return False

    def _on_resize(self, _widget, alloc) -> None:
        size = (alloc.width, alloc.height)
        if size != self._last_alloc:
            self._last_alloc = size
            self._refresh()

    def _nav(self, delta: int) -> None:
        n = len(self.pairs)
        if n == 0:
            return
        self.page_idx = max(0, min(n - 1, self.page_idx + delta))
        self._refresh()

    def _refresh(self) -> None:
        if not self.pairs:
            self.header.set_text("No pages to display.")
            return
        label, orig, rend = self.pairs[self.page_idx]
        self.header.set_markup(
            f"<b>{GLib.markup_escape_text(label)}</b>   "
            f"({self.page_idx + 1} / {len(self.pairs)})   — "
            f"Left/Right arrows to navigate, Q to quit"
        )

        alloc_w, alloc_h = self._last_alloc
        if alloc_w == 0 or alloc_h == 0:
            alloc_w, alloc_h = 1600, 900
        pane_w = max(200, alloc_w // 2 - 40)
        pane_h = max(200, alloc_h - 160)

        for pil_img, gtk_img in (
            (orig, self.orig_image),
            (rend, self.rend_image),
        ):
            w, h = pil_img.size
            scale = min(pane_w / w, pane_h / h, 1.0)
            nw, nh = max(1, int(w * scale)), max(1, int(h * scale))
            scaled = pil_img.resize((nw, nh), Image.LANCZOS)
            gtk_img.set_from_pixbuf(_pil_to_pixbuf(scaled))


def _load_photo(path: Path) -> Image.Image:
    img = Image.open(path)
    img = ImageOps.exif_transpose(img).convert("RGB")
    return img


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Compare a homework<N>.md against its source page photo(s) in GTK.",
    )
    parser.add_argument(
        "md", metavar="HOMEWORK.MD",
        help="Path to a homework<N>.md challenge file.",
    )
    parser.add_argument(
        "images", nargs="+", metavar="IMAGE",
        help="Original page photo(s), one per Page in the markdown.",
    )
    parser.add_argument(
        "--board-size", type=int, default=320,
        help="Pixel size of each rendered board in the composite (default 320).",
    )
    args = parser.parse_args()

    md_path = Path(args.md).expanduser().resolve()
    if not md_path.is_file():
        print(f"Error: homework file not found: {md_path}", file=sys.stderr)
        sys.exit(1)

    pages = parse_homework_md(md_path)
    if not pages:
        print(f"Error: no FEN positions found in {md_path}", file=sys.stderr)
        sys.exit(1)

    if len(args.images) != len(pages):
        print(
            f"Note: {len(pages)} page(s) in {md_path.name} but "
            f"{len(args.images)} photo(s) given — using first "
            f"{min(len(pages), len(args.images))}.",
            file=sys.stderr,
        )

    pairs: list[tuple[str, Image.Image, Image.Image]] = []
    for idx, (page, img_path) in enumerate(zip(pages, args.images)):
        try:
            photo = _load_photo(Path(img_path).expanduser())
        except Exception as e:
            print(f"Skipping {img_path}: {e}", file=sys.stderr)
            continue
        rendered = _compose_page(
            page, board_size=args.board_size,
            title=f"{md_path.stem} — Page {idx + 1}",
        )
        pairs.append((f"{md_path.stem} — Page {idx + 1}", photo, rendered))

    if not pairs:
        print("Error: nothing to display.", file=sys.stderr)
        sys.exit(1)

    win = HomeworkViewer(pairs)
    win.connect("destroy", Gtk.main_quit)
    win.show_all()
    Gtk.main()


if __name__ == "__main__":
    main()
