#!/usr/bin/env python3
"""
Unified GTK wizard for turning a folder of chess homework photos
into a ``homework<N>.md`` challenge file.

Flow:
  1. Launch the window → pick a directory of page photos.
  2. The tool runs every image through Gemini (`page_to_fens`) on a
     background thread, updating a progress bar.
  3. A review screen shows each photo side-by-side with a composite
     rendering of the extracted FENs, one page at a time.
  4. Accept → the FENs are written to the next unused
     ``challenges/homework<N>.md``. Cancel → back to step 1.

Requires:
    pip install google-genai Pillow
    GOOGLE_API_KEY or GEMINI_API_KEY in the environment
    python3-gi + gir1.2-gtk-3.0 (GTK 3 bindings)
"""

import io
import os
import sys
import threading
from pathlib import Path

try:
    import gi
    gi.require_version("Gtk", "3.0")
    from gi.repository import Gtk, Gdk, GdkPixbuf, GLib  # noqa: F401
except ImportError:
    print(
        "Error: PyGObject with GTK 3 is required.\n"
        "  Debian/Ubuntu: sudo apt install python3-gi gir1.2-gtk-3.0",
        file=sys.stderr,
    )
    sys.exit(1)

try:
    from PIL import Image, ImageOps
except ImportError:
    print("Error: Pillow is required.  pip install Pillow", file=sys.stderr)
    sys.exit(1)

sys.path.insert(0, str(Path(__file__).resolve().parent))
from image_to_fen import (  # noqa: E402
    page_to_fens,
    write_homework_md,
    _next_homework_path,
)
from fen_to_images import _compose_page  # noqa: E402


_IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".webp", ".gif"}


def _pil_to_pixbuf(img: Image.Image) -> GdkPixbuf.Pixbuf:
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    loader = GdkPixbuf.PixbufLoader.new_with_type("png")
    loader.write(buf.getvalue())
    loader.close()
    return loader.get_pixbuf()


class HomeworkWizard(Gtk.Window):
    def __init__(self) -> None:
        super().__init__(title="Homework Wizard")
        self.set_default_size(1600, 900)

        # Each entry: (photo_path, photo_pil, [(label, fen), ...])
        self.pages: list[tuple[Path, Image.Image, list[tuple[str, str]]]] = []
        self.page_idx = 0
        self._last_alloc = (0, 0)

        self.stack = Gtk.Stack()
        self.stack.set_transition_type(Gtk.StackTransitionType.CROSSFADE)
        self.add(self.stack)

        self._build_welcome()
        self._build_loading()
        self._build_review()
        self._build_saved()

        self.stack.set_visible_child_name("welcome")

        self.connect("key-press-event", self._on_key)
        self.connect("size-allocate", self._on_resize)

    # ─── Screens ────────────────────────────────────────────────────

    def _build_welcome(self) -> None:
        v = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=24)
        v.set_margin_top(80); v.set_margin_bottom(80)
        v.set_margin_start(80); v.set_margin_end(80)
        v.set_valign(Gtk.Align.CENTER)

        title = Gtk.Label()
        title.set_markup(
            "<span size='xx-large' weight='bold'>Homework Wizard</span>"
        )
        v.pack_start(title, False, False, 0)

        subtitle = Gtk.Label(label=(
            "Pick a directory of chess board photos.\n"
            "Each image becomes one Page in the resulting "
            "homework<N>.md challenge file."
        ))
        subtitle.set_justify(Gtk.Justification.CENTER)
        v.pack_start(subtitle, False, False, 0)

        btn = Gtk.Button(label="Select directory…")
        btn.set_size_request(260, 60)
        btn.set_halign(Gtk.Align.CENTER)
        btn.get_style_context().add_class("suggested-action")
        btn.connect("clicked", lambda *_: self._pick_directory())
        v.pack_start(btn, False, False, 0)

        self.api_warning = Gtk.Label()
        self.api_warning.set_justify(Gtk.Justification.CENTER)
        self._refresh_api_warning()
        v.pack_start(self.api_warning, False, False, 0)

        self.stack.add_named(v, "welcome")

    def _refresh_api_warning(self) -> None:
        has_key = bool(
            os.environ.get("GOOGLE_API_KEY")
            or os.environ.get("GEMINI_API_KEY")
        )
        if has_key:
            self.api_warning.set_text("")
        else:
            self.api_warning.set_markup(
                "<span foreground='#b00000'>Warning: "
                "GOOGLE_API_KEY / GEMINI_API_KEY is not set. "
                "Set it before selecting a directory.</span>"
            )

    def _build_loading(self) -> None:
        v = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=20)
        v.set_valign(Gtk.Align.CENTER)
        v.set_halign(Gtk.Align.CENTER)

        self.loading_label = Gtk.Label(label="Processing…")
        v.pack_start(self.loading_label, False, False, 0)

        self.progress = Gtk.ProgressBar()
        self.progress.set_size_request(500, 24)
        v.pack_start(self.progress, False, False, 0)

        self.stack.add_named(v, "loading")

    def _build_review(self) -> None:
        v = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        v.set_margin_top(8); v.set_margin_bottom(8)
        v.set_margin_start(8); v.set_margin_end(8)

        self.header = Gtk.Label()
        self.header.set_xalign(0.5)
        v.pack_start(self.header, False, False, 0)

        hbox = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        v.pack_start(hbox, True, True, 0)

        self.orig_image = Gtk.Image()
        self.rend_image = Gtk.Image()
        left_frame = Gtk.Frame(label="Original photo")
        left_frame.add(self.orig_image)
        right_frame = Gtk.Frame(label="Extracted positions")
        right_frame.add(self.rend_image)
        hbox.pack_start(left_frame, True, True, 0)
        hbox.pack_start(right_frame, True, True, 0)

        nav = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        prev_btn = Gtk.Button(label="◀ Prev")
        prev_btn.connect("clicked", lambda *_: self._nav(-1))
        next_btn = Gtk.Button(label="Next ▶")
        next_btn.connect("clicked", lambda *_: self._nav(1))
        nav.pack_start(prev_btn, True, True, 0)
        nav.pack_start(next_btn, True, True, 0)
        v.pack_start(nav, False, False, 0)

        actions = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        cancel_btn = Gtk.Button(label="Cancel")
        cancel_btn.connect("clicked", lambda *_: self._to_welcome())
        accept_btn = Gtk.Button(label="Accept — save homework<N>.md")
        accept_btn.get_style_context().add_class("suggested-action")
        accept_btn.connect("clicked", lambda *_: self._save())
        actions.pack_start(cancel_btn, True, True, 0)
        actions.pack_start(accept_btn, True, True, 0)
        v.pack_start(actions, False, False, 0)

        self.stack.add_named(v, "review")

    def _build_saved(self) -> None:
        v = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=20)
        v.set_valign(Gtk.Align.CENTER)
        v.set_halign(Gtk.Align.CENTER)

        self.saved_label = Gtk.Label()
        self.saved_label.set_justify(Gtk.Justification.CENTER)
        v.pack_start(self.saved_label, False, False, 0)

        again_btn = Gtk.Button(label="Process another directory")
        again_btn.connect("clicked", lambda *_: self._to_welcome())
        v.pack_start(again_btn, False, False, 0)

        quit_btn = Gtk.Button(label="Quit")
        quit_btn.connect("clicked", lambda *_: Gtk.main_quit())
        v.pack_start(quit_btn, False, False, 0)

        self.stack.add_named(v, "saved")

    # ─── Transitions ────────────────────────────────────────────────

    def _to_welcome(self) -> None:
        self._refresh_api_warning()
        self.stack.set_visible_child_name("welcome")

    def _pick_directory(self) -> None:
        dlg = Gtk.FileChooserDialog(
            title="Choose a directory of chess board photos",
            parent=self,
            action=Gtk.FileChooserAction.SELECT_FOLDER,
        )
        dlg.add_buttons(
            "Cancel", Gtk.ResponseType.CANCEL,
            "Open", Gtk.ResponseType.OK,
        )
        resp = dlg.run()
        path = dlg.get_filename() if resp == Gtk.ResponseType.OK else None
        dlg.destroy()
        if path:
            self._start_processing(Path(path))

    def _start_processing(self, directory: Path) -> None:
        images = sorted(
            p for p in directory.iterdir()
            if p.is_file() and p.suffix.lower() in _IMAGE_EXTS
        )
        if not images:
            self._show_error(f"No images found in {directory}")
            return

        self.pages = []
        self.page_idx = 0
        self.stack.set_visible_child_name("loading")
        self.progress.set_fraction(0.0)
        self.loading_label.set_text(f"Processing 0/{len(images)}…")

        thread = threading.Thread(
            target=self._process_thread, args=(images,), daemon=True,
        )
        thread.start()

    def _process_thread(
        self, images: list[Path]
    ) -> None:
        """Run on a background thread so the UI stays responsive."""
        results: list[tuple[Path, Image.Image, list[tuple[str, str]]]] = []
        total = len(images)
        for idx, img_path in enumerate(images):
            try:
                entries = page_to_fens(str(img_path))
            except Exception as e:
                GLib.idle_add(
                    self._show_error, f"{img_path.name}: {e}"
                )
                return
            try:
                photo = Image.open(img_path)
                photo = ImageOps.exif_transpose(photo).convert("RGB")
            except Exception as e:
                GLib.idle_add(
                    self._show_error, f"{img_path.name}: {e}"
                )
                return
            results.append((img_path, photo, entries))
            GLib.idle_add(self._progress_update, idx + 1, total)

        GLib.idle_add(self._on_processing_done, results)

    def _progress_update(self, done: int, total: int) -> bool:
        self.progress.set_fraction(done / total)
        self.loading_label.set_text(f"Processed {done}/{total} images")
        return False

    def _on_processing_done(
        self,
        results: list[tuple[Path, Image.Image, list[tuple[str, str]]]],
    ) -> bool:
        self.pages = results
        self.page_idx = 0
        self.stack.set_visible_child_name("review")
        self._refresh_review()
        return False

    # ─── Review screen ──────────────────────────────────────────────

    def _nav(self, delta: int) -> None:
        if not self.pages:
            return
        self.page_idx = max(
            0, min(len(self.pages) - 1, self.page_idx + delta)
        )
        self._refresh_review()

    def _refresh_review(self) -> None:
        if not self.pages:
            return
        img_path, photo, entries = self.pages[self.page_idx]
        self.header.set_markup(
            "<b>{name}</b>   (Page {i} / {n}  —  {k} board(s))".format(
                name=GLib.markup_escape_text(img_path.name),
                i=self.page_idx + 1, n=len(self.pages), k=len(entries),
            )
        )

        alloc_w, alloc_h = self._last_alloc
        if alloc_w == 0 or alloc_h == 0:
            alloc_w, alloc_h = 1600, 900
        pane_w = max(200, alloc_w // 2 - 40)
        pane_h = max(200, alloc_h - 240)

        rendered = _compose_page(
            entries, board_size=260,
            title=f"Page {self.page_idx + 1}",
        )

        for pil_img, gtk_img in (
            (photo, self.orig_image),
            (rendered, self.rend_image),
        ):
            w, h = pil_img.size
            scale = min(pane_w / w, pane_h / h, 1.0)
            nw, nh = max(1, int(w * scale)), max(1, int(h * scale))
            scaled = pil_img.resize((nw, nh), Image.LANCZOS)
            gtk_img.set_from_pixbuf(_pil_to_pixbuf(scaled))

    # ─── Save + errors ──────────────────────────────────────────────

    def _save(self) -> None:
        if not self.pages:
            return
        pages_md = [entries for _, _, entries in self.pages]
        out = _next_homework_path()
        write_homework_md(pages_md, out)
        n_boards = sum(len(e) for e in pages_md)
        self.saved_label.set_markup(
            "<span size='x-large'>Saved "
            f"<b>{n_boards}</b> FEN(s) across "
            f"<b>{len(pages_md)}</b> page(s) to:\n"
            f"<tt>{GLib.markup_escape_text(str(out))}</tt></span>"
        )
        self.stack.set_visible_child_name("saved")

    def _show_error(self, msg: str) -> bool:
        dlg = Gtk.MessageDialog(
            transient_for=self,
            modal=True,
            message_type=Gtk.MessageType.ERROR,
            buttons=Gtk.ButtonsType.OK,
            text="Error",
        )
        dlg.format_secondary_text(msg)
        dlg.run()
        dlg.destroy()
        self._to_welcome()
        return False

    # ─── Event handlers ─────────────────────────────────────────────

    def _on_key(self, _widget, event) -> bool:
        if self.stack.get_visible_child_name() == "review":
            if event.keyval in (Gdk.KEY_Left, Gdk.KEY_Page_Up):
                self._nav(-1); return True
            if event.keyval in (Gdk.KEY_Right, Gdk.KEY_Page_Down, Gdk.KEY_space):
                self._nav(1); return True
        if event.keyval in (Gdk.KEY_q, Gdk.KEY_Escape):
            Gtk.main_quit()
            return True
        return False

    def _on_resize(self, _widget, alloc) -> None:
        size = (alloc.width, alloc.height)
        if size != self._last_alloc:
            self._last_alloc = size
            if self.stack.get_visible_child_name() == "review":
                self._refresh_review()


def main() -> None:
    win = HomeworkWizard()
    win.connect("destroy", Gtk.main_quit)
    win.show_all()
    Gtk.main()


if __name__ == "__main__":
    main()
