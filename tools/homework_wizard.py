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
    _render_fen_image,
    DEFAULT_PUZZLE_TYPE,
    KNOWN_PUZZLE_TYPES,
)


_IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".webp", ".gif"}
_BOARD_PX = 320  # rendered board size for the review grid

# FEN letters shown in the piece picker popover.
_PIECES = [
    ("K", "K"), ("Q", "Q"), ("R", "R"),
    ("B", "B"), ("N", "N"), ("P", "P"),
]


def _expand_placement(placement: str) -> list[list[str]]:
    """FEN placement → 8x8 grid of single-char codes ('.' for empty)."""
    ranks = placement.split("/")
    board = [["."] * 8 for _ in range(8)]
    for r, rank_str in enumerate(ranks):
        c = 0
        for ch in rank_str:
            if ch.isdigit():
                c += int(ch)
            else:
                if 0 <= c < 8 and 0 <= r < 8:
                    board[r][c] = ch
                c += 1
    return board


def _pack_placement(board: list[list[str]]) -> str:
    """8x8 grid → FEN placement string."""
    out: list[str] = []
    for r in range(8):
        s = ""
        empty = 0
        for c in range(8):
            ch = board[r][c]
            if ch == ".":
                empty += 1
            else:
                if empty:
                    s += str(empty)
                    empty = 0
                s += ch
        if empty:
            s += str(empty)
        out.append(s)
    return "/".join(out)


def _edit_fen(fen: str, row: int, col: int, piece: str) -> str:
    """Return a new FEN with the (row, col) square replaced by
    ``piece`` (single char like 'K', 'k', ... or '.' for empty).

    ``row`` is 0 at the top (rank 8), ``col`` is 0 at the left (file a).
    The side-to-move / castling / etc. metadata is preserved."""
    parts = fen.split(maxsplit=1)
    placement = parts[0]
    rest = parts[1] if len(parts) > 1 else "w - - 0 1"

    board = _expand_placement(placement)
    board[row][col] = piece
    return f"{_pack_placement(board)} {rest}"


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
        # Parallel list: per-page puzzle type (user-editable in the
        # review screen via the type combo box).
        self.page_types: list[str] = []
        self.page_idx = 0
        self._last_alloc = (0, 0)
        # Set when the combo box is being programmatically updated so
        # its changed-signal doesn't fight our own state writes.
        self._suppress_type_signal = False

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

        # Per-page puzzle type selector.
        type_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        type_row.set_halign(Gtk.Align.CENTER)
        type_row.pack_start(Gtk.Label(label="Puzzle type:"), False, False, 0)
        self.type_combo = Gtk.ComboBoxText()
        for t in KNOWN_PUZZLE_TYPES:
            self.type_combo.append_text(t)
        self.type_combo.connect("changed", self._on_type_changed)
        type_row.pack_start(self.type_combo, False, False, 0)
        v.pack_start(type_row, False, False, 0)

        hint = Gtk.Label()
        hint.set_markup(
            "<i>Click a square to edit a single piece · drag to move a piece.</i>"
        )
        hint.set_xalign(0.5)
        v.pack_start(hint, False, False, 0)

        hbox = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        v.pack_start(hbox, True, True, 0)

        # Left pane: scrollable original photo.
        self.orig_image = Gtk.Image()
        left_scroll = Gtk.ScrolledWindow()
        left_scroll.add(self.orig_image)
        left_frame = Gtk.Frame(label="Original photo")
        left_frame.add(left_scroll)
        hbox.pack_start(left_frame, True, True, 0)

        # Right pane: scrollable grid of clickable boards.
        self.rend_grid = Gtk.Grid()
        self.rend_grid.set_row_spacing(12)
        self.rend_grid.set_column_spacing(12)
        self.rend_grid.set_halign(Gtk.Align.CENTER)
        # Per-board state populated by _refresh_review.
        self._board_widgets: list[dict] = []

        right_scroll = Gtk.ScrolledWindow()
        right_scroll.add(self.rend_grid)
        right_frame = Gtk.Frame(label="Extracted positions (click to edit)")
        right_frame.add(right_scroll)
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
        """Process images sequentially on a background thread.

        Per-page board extraction is already parallelized internally
        by ``page_to_fens``; we intentionally do **not** fan out at
        the page level on top of that. Early experiments showed that
        firing ~12 concurrent Gemini requests (2 pages × 6 boards)
        reliably produced worse-quality FENs, likely because of
        provider-side throttling or batch effects."""
        total = len(images)
        results: list[tuple[Path, Image.Image, list[tuple[str, str]]]] = []
        for idx, img_path in enumerate(images):
            try:
                entries = page_to_fens(str(img_path))
                photo = Image.open(img_path)
                photo = ImageOps.exif_transpose(photo).convert("RGB")
            except Exception as e:
                GLib.idle_add(self._show_error, f"{img_path.name}: {e}")
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
        self.page_types = [DEFAULT_PUZZLE_TYPE] * len(results)
        self.page_idx = 0
        self.stack.set_visible_child_name("review")
        self._refresh_review()
        return False

    def _on_type_changed(self, combo: Gtk.ComboBoxText) -> None:
        if self._suppress_type_signal or not self.pages:
            return
        t = combo.get_active_text()
        if t:
            self.page_types[self.page_idx] = t

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

        # Sync the type combo to the current page without triggering
        # our own changed-handler.
        cur_type = self.page_types[self.page_idx]
        self._suppress_type_signal = True
        try:
            model = self.type_combo.get_model()
            active_idx = next(
                (i for i, row in enumerate(model) if row[0] == cur_type),
                -1,
            )
            if active_idx < 0:
                self.type_combo.append_text(cur_type)
                active_idx = len(model)
            self.type_combo.set_active(active_idx)
        finally:
            self._suppress_type_signal = False

        alloc_w, alloc_h = self._last_alloc
        if alloc_w == 0 or alloc_h == 0:
            alloc_w, alloc_h = 1600, 900
        pane_w = max(200, alloc_w // 2 - 40)
        pane_h = max(200, alloc_h - 260)

        # Left: photo scaled to fit.
        w, h = photo.size
        scale = min(pane_w / w, pane_h / h, 1.0)
        nw, nh = max(1, int(w * scale)), max(1, int(h * scale))
        self.orig_image.set_from_pixbuf(
            _pil_to_pixbuf(photo.resize((nw, nh), Image.LANCZOS))
        )

        # Right: clickable board grid. Rebuild from scratch each
        # refresh so navigation between pages shows the right set.
        for child in self.rend_grid.get_children():
            self.rend_grid.remove(child)
        self._board_widgets = []

        cols = 3 if len(entries) > 2 else max(1, len(entries))
        for idx, (label, fen) in enumerate(entries):
            r, c = divmod(idx, cols)

            vbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
            vbox.set_halign(Gtk.Align.CENTER)

            lbl = Gtk.Label()
            lbl.set_markup(f"<b>{GLib.markup_escape_text(label)}</b>")
            vbox.pack_start(lbl, False, False, 0)

            ebox = Gtk.EventBox()
            ebox.set_visible_window(False)
            ebox.add_events(
                Gdk.EventMask.BUTTON_PRESS_MASK
                | Gdk.EventMask.BUTTON_RELEASE_MASK
            )
            gimg = Gtk.Image()
            ebox.add(gimg)
            vbox.pack_start(ebox, False, False, 0)

            self.rend_grid.attach(vbox, c, r, 1, 1)

            state = {
                "board_idx": idx, "gtk_image": gimg,
                "event_box": ebox, "board_px": _BOARD_PX,
                "drag_start": None,
            }
            self._board_widgets.append(state)
            ebox.connect("button-press-event", self._on_board_press, idx)
            ebox.connect("button-release-event", self._on_board_release, idx)

            self._redraw_board(idx)

        self.rend_grid.show_all()

    def _redraw_board(self, board_idx: int) -> None:
        """Re-render a single board's Gtk.Image from its current FEN."""
        _, _, entries = self.pages[self.page_idx]
        _, fen = entries[board_idx]
        try:
            pil = _render_fen_image(fen, size=_BOARD_PX)
        except ValueError:
            pil = Image.new("RGB", (_BOARD_PX, _BOARD_PX), (240, 200, 200))
        self._board_widgets[board_idx]["gtk_image"].set_from_pixbuf(
            _pil_to_pixbuf(pil)
        )

    def _square_from_coords(self, x: float, y: float) -> tuple[int, int] | None:
        """Map a pixel coordinate inside a rendered board to (row, col).
        Returns ``None`` if the point falls outside the 8x8 grid."""
        px = _BOARD_PX
        margin = px // 20
        cell = (px - 2 * margin) // 8
        bx = int(x) - margin
        by = int(y) - margin
        col = bx // cell
        row = by // cell
        if 0 <= col < 8 and 0 <= row < 8:
            return row, col
        return None

    def _on_board_press(self, _event_box, event, board_idx: int) -> bool:
        """Remember where a mouse-press happened so the release handler
        can decide between 'click → edit' and 'drag → move'."""
        sq = self._square_from_coords(event.x, event.y)
        self._board_widgets[board_idx]["drag_start"] = sq
        return False

    def _on_board_release(self, event_box, event, board_idx: int) -> bool:
        """Route release events: same square → open piece picker,
        different square → move the piece."""
        state = self._board_widgets[board_idx]
        start = state["drag_start"]
        state["drag_start"] = None
        if start is None:
            return False

        end = self._square_from_coords(event.x, event.y)
        if end is None or end == start:
            self._show_piece_picker(
                state["event_box"], board_idx, start[0], start[1]
            )
            return True

        self._apply_move(board_idx, start, end)
        return True

    def _apply_move(
        self,
        board_idx: int,
        src: tuple[int, int],
        dst: tuple[int, int],
    ) -> None:
        """Move the piece at ``src`` to ``dst`` (replacing any piece
        already there). No-op if ``src`` is empty."""
        _, _, entries = self.pages[self.page_idx]
        label, fen = entries[board_idx]
        parts = fen.split(maxsplit=1)
        placement = parts[0]
        rest = parts[1] if len(parts) > 1 else "w - - 0 1"

        board = _expand_placement(placement)
        piece = board[src[0]][src[1]]
        if piece == ".":
            return
        board[src[0]][src[1]] = "."
        board[dst[0]][dst[1]] = piece
        entries[board_idx] = (label, f"{_pack_placement(board)} {rest}")
        self._redraw_board(board_idx)

    def _show_piece_picker(
        self, anchor: Gtk.Widget, board_idx: int, row: int, col: int
    ) -> None:
        """Pop up a small grid of piece buttons. White row on top,
        black row below, plus a Delete button."""
        pop = Gtk.Popover.new(anchor)
        pop.set_position(Gtk.PositionType.BOTTOM)

        grid = Gtk.Grid()
        grid.set_row_spacing(4)
        grid.set_column_spacing(4)
        grid.set_margin_top(8); grid.set_margin_bottom(8)
        grid.set_margin_start(8); grid.set_margin_end(8)

        hdr = Gtk.Label()
        file_letter = "abcdefgh"[col]
        rank_digit = 8 - row
        hdr.set_markup(
            f"<b>{file_letter}{rank_digit}</b> — choose piece"
        )
        grid.attach(hdr, 0, 0, len(_PIECES), 1)

        grid.attach(Gtk.Label(label="White"), 0, 1, len(_PIECES), 1)
        for c, (code, glyph) in enumerate(_PIECES):
            btn = Gtk.Button(label=glyph)
            btn.set_size_request(44, 44)
            btn.connect(
                "clicked",
                lambda _b, ch=code.upper(): self._apply_edit(
                    pop, board_idx, row, col, ch
                ),
            )
            grid.attach(btn, c, 2, 1, 1)

        grid.attach(Gtk.Label(label="Black"), 0, 3, len(_PIECES), 1)
        for c, (code, glyph) in enumerate(_PIECES):
            btn = Gtk.Button(label=glyph.lower())
            btn.set_size_request(44, 44)
            btn.connect(
                "clicked",
                lambda _b, ch=code.lower(): self._apply_edit(
                    pop, board_idx, row, col, ch
                ),
            )
            grid.attach(btn, c, 4, 1, 1)

        clear_btn = Gtk.Button(label="✕ Clear square")
        clear_btn.connect(
            "clicked",
            lambda _b: self._apply_edit(pop, board_idx, row, col, "."),
        )
        grid.attach(clear_btn, 0, 5, len(_PIECES), 1)

        pop.add(grid)
        pop.show_all()
        pop.popup()

    def _apply_edit(
        self,
        popover: Gtk.Popover,
        board_idx: int,
        row: int,
        col: int,
        piece: str,
    ) -> None:
        _, _, entries = self.pages[self.page_idx]
        label, fen = entries[board_idx]
        entries[board_idx] = (label, _edit_fen(fen, row, col, piece))
        self._redraw_board(board_idx)
        popover.popdown()

    # ─── Save + errors ──────────────────────────────────────────────

    def _save(self) -> None:
        if not self.pages:
            return

        default = _next_homework_path()
        dlg = Gtk.FileChooserDialog(
            title="Save challenge file",
            parent=self,
            action=Gtk.FileChooserAction.SAVE,
        )
        dlg.add_buttons(
            "Cancel", Gtk.ResponseType.CANCEL,
            "Save", Gtk.ResponseType.OK,
        )
        dlg.set_do_overwrite_confirmation(True)
        try:
            dlg.set_current_folder(str(default.parent.resolve()))
        except Exception:
            pass
        dlg.set_current_name(default.name)

        resp = dlg.run()
        chosen = dlg.get_filename() if resp == Gtk.ResponseType.OK else None
        dlg.destroy()
        if not chosen:
            return

        out = Path(chosen)
        if not out.suffix:
            out = out.with_suffix(".md")

        pages_md = [
            {
                "type": self.page_types[i],
                "side": "white",
                "entries": entries,
            }
            for i, (_, _, entries) in enumerate(self.pages)
        ]
        write_homework_md(pages_md, out)
        n_boards = sum(len(p["entries"]) for p in pages_md)
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
