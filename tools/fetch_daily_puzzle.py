#!/usr/bin/env python3
"""
Fetch chess.com puzzles and archive them under ../puzzles/.

Each run hits two endpoints:
    * /pub/puzzle           → today's chess.com Puzzle of the Day
    * /pub/puzzle/random    → an arbitrary historical puzzle

Both responses land directly in ../puzzles/ as flat files.
Filenames are derived from the puzzle's `title` (sanitised to
[A-Za-z0-9._-]_); if the title is empty we fall back to today's
date plus a short FEN-derived suffix. Dedup is by FEN — if any
existing file under puzzles/*.md already contains the response's
FEN as a standalone line, that fetch is skipped. Same puzzle
never gets two files even when chess.com reposts it later or
random happens to surface the day's daily.

The written file mirrors challenges/*.md (name + type + side +
FEN, with the chess.com solution PGN appended as a comment
block) so it stays parseable as a single-page challenge.

When a new file IS written, the script also commits it as its
own single-file commit and pushes to the configured origin via
the local `git` binary. Authentication is whatever git is
already configured to use on this machine (SSH key, credential
helper, or the URL stored in .git/config). The script never
reads or embeds any secret of its own — no environment variables
are inspected, no tokens hardcoded, no stdout / stderr from git
is re-emitted beyond a short summary line. Pass --no-push to
skip the commit + push step (useful for local testing or for
clones in read-only environments).

Suggested crontab line — run every 3 h, offset by 1 h so the
first fire of the day is 01:00 local:
    0 1-23/3 * * * /path/to/3d_chess/tools/fetch_daily_puzzle.py >> /tmp/fetch_daily_puzzle.log 2>&1
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.request
import urllib.error

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
PUZZLES_DIR = os.path.join(PROJECT_ROOT, "puzzles")
ENDPOINTS = {
    "daily":  "https://api.chess.com/pub/puzzle",
    "random": "https://api.chess.com/pub/puzzle/random",
}
USER_AGENT = "3d-chess fetch_daily_puzzle.py (cron)"
TIMEOUT_S = 15


def log(msg: str) -> None:
    """Timestamp + write to stderr so cron's stderr-on-failure mail surfaces it."""
    ts = time.strftime("%Y-%m-%dT%H:%M:%S")
    print(f"[{ts}] {msg}", file=sys.stderr)


def fetch_puzzle(endpoint: str) -> dict:
    req = urllib.request.Request(endpoint, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=TIMEOUT_S) as r:
        if r.status != 200:
            raise RuntimeError(f"HTTP {r.status} from {endpoint}")
        body = r.read().decode("utf-8", errors="replace")
    return json.loads(body)


def fen_already_archived(fen: str) -> bool:
    """True if any *.md in puzzles/ contains the FEN as a standalone
    line. The archive writer always emits the FEN on its own line, so
    a literal-string match is reliable."""
    if not os.path.isdir(PUZZLES_DIR):
        return False
    fen_line = fen.strip()
    for name in os.listdir(PUZZLES_DIR):
        if not name.endswith(".md"):
            continue
        path = os.path.join(PUZZLES_DIR, name)
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    if line.strip() == fen_line:
                        return True
        except OSError:
            continue
    return False


def slugify(s: str) -> str:
    """Title / URL-tail → safe POSIX filename component."""
    return re.sub(r"[^A-Za-z0-9._-]+", "_", s).strip("_")


def side_from_fen(fen: str) -> str:
    parts = fen.split()
    return "black" if len(parts) >= 2 and parts[1] == "b" else "white"


_URL_DATE_RE = re.compile(r"/(\d{4}-\d{2}-\d{2})(?:/|$|\?)")


def url_date(url: str, fallback: str) -> str:
    """Pull the YYYY-MM-DD chunk out of a chess.com puzzle URL like
    https://www.chess.com/daily/2019-12-09 . Falls back to `fallback`
    when the URL is missing the date or empty."""
    if not url:
        return fallback
    m = _URL_DATE_RE.search(url)
    return m.group(1) if m else fallback


def archive_path_for(title: str, fen: str, url: str, today: str) -> str:
    """Pick a non-clobbering path under PUZZLES_DIR/. Filename is
    `<sanitised-title>_<url-date>.md` — `url-date` is the YYYY-MM-DD
    embedded in the chess.com URL (so a random puzzle from 2019
    archives under its real publish date, not when we happened to
    fetch it). If the URL doesn't carry a date we fall back to
    today; if the title is empty we use `<url-date>_<short-fen-
    hash>.md` so different positions on the same day stay distinct."""
    date = url_date(url, today)
    title_slug = slugify(title)[:80].rstrip("_") if title else ""
    if title_slug:
        base = f"{title_slug}_{date}"
    else:
        h = abs(hash(fen)) % 0x10000
        base = f"{date}_{h:04x}"
    path = os.path.join(PUZZLES_DIR, base + ".md")
    suffix = 2
    while os.path.exists(path):
        path = os.path.join(PUZZLES_DIR, f"{base}_{suffix}.md")
        suffix += 1
    return path


_PGN_TAG_RE = re.compile(r"^\s*\[[^\]]*\]\s*$", re.MULTILINE)


def pgn_move_list(pgn: str) -> str:
    """Strip `[Tag "value"]` headers from a PGN body and collapse
    whitespace to one line. The result is the SAN move sequence
    (e.g. "1. e4 e5 2. Nf3 *") that we store under `solution:`."""
    if not pgn:
        return ""
    body = _PGN_TAG_RE.sub("", pgn)
    return " ".join(body.split())


def write_archive(p: dict, kind: str, today: str) -> str:
    fen   = p["fen"]
    title = p.get("title") or ""
    url   = p.get("url")   or ""
    pgn   = p.get("pgn")   or ""

    os.makedirs(PUZZLES_DIR, exist_ok=True)
    path = archive_path_for(title, fen, url, today)

    label = "Daily" if kind == "daily" else "Random"
    moves = pgn_move_list(pgn)
    lines: list[str] = []
    lines.append(f"# Chess.com {label} Puzzle archive")
    lines.append("#")
    lines.append(f"# Fetched on {today} by tools/fetch_daily_puzzle.py")
    lines.append("")
    if title:
        lines.append(f"name: {title}")
    if url:
        lines.append(f"url: {url}")
    lines.append("")
    lines.append("type: puzzle")
    lines.append(f"side: {side_from_fen(fen)}")
    lines.append(fen)
    if moves:
        lines.append(f"solution: {moves}")

    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return path


def _git(*args: str, timeout: int = 30) -> subprocess.CompletedProcess:
    """Wrapper around `git -C PROJECT_ROOT ...` that captures output.

    No secrets are passed in — git auth is handled by whatever
    credential helper / SSH agent is already configured on this
    host. We only log return codes and a short stderr excerpt so
    push diagnostics don't drown cron mail in noise (and so we
    don't accidentally surface anything sensitive that a custom
    credential helper might choose to print). The caller decides
    whether to treat non-zero rc as fatal."""
    cmd = ["git", "-C", PROJECT_ROOT, *args]
    return subprocess.run(cmd, capture_output=True, timeout=timeout, text=True)


def git_commit_and_push(path: str) -> None:
    """Commit just `path` and push to origin. Best-effort.

    We stage exactly one file with `git add <path>` so that any
    other in-progress edits in the working tree don't piggyback
    onto the puzzle commit. Errors are logged but never raised —
    a push failure shouldn't block the next archive write."""
    rel = os.path.relpath(path, PROJECT_ROOT)
    try:
        add = _git("add", "--", rel)
        if add.returncode != 0:
            log(f"git add failed (rc={add.returncode}): "
                f"{(add.stderr or '').strip()[:200]}")
            return
        # The committer / author identity comes from the user's
        # ~/.gitconfig — we do NOT inject one. If git is configured
        # without a name + email it will refuse the commit and we
        # log the failure cleanly.
        msg = f"puzzles: archive {os.path.basename(path)}"
        commit = _git("commit", "-m", msg)
        if commit.returncode != 0:
            log(f"git commit failed (rc={commit.returncode}): "
                f"{(commit.stderr or '').strip()[:200]}")
            return
        push = _git("push", timeout=60)
        if push.returncode != 0:
            log(f"git push failed (rc={push.returncode}): "
                f"{(push.stderr or '').strip()[:200]}")
            return
        log(f"committed + pushed {rel}")
    except FileNotFoundError:
        log("git binary not found in PATH — skip commit/push")
    except subprocess.TimeoutExpired:
        log("git step timed out — skip")


def fetch_one(kind: str, today: str, push: bool) -> int:
    """Fetch a single endpoint, dedup, write + commit if new. Returns
    a process-style exit code (0 = ok, 1 = transient failure that
    shouldn't poison the other endpoint's run)."""
    endpoint = ENDPOINTS[kind]
    try:
        p = fetch_puzzle(endpoint)
    except (urllib.error.URLError, OSError, json.JSONDecodeError) as e:
        log(f"[{kind}] fetch failed: {e}")
        return 1

    fen = (p.get("fen") or "").strip()
    if not fen:
        log(f"[{kind}] response had no FEN; skipping")
        return 1

    if fen_already_archived(fen):
        log(f"[{kind}] already archived (FEN match) — skip: {fen}")
        return 0

    path = write_archive(p, kind, today)
    log(f"[{kind}] wrote {os.path.relpath(path, PROJECT_ROOT)}")
    if push:
        git_commit_and_push(path)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--no-push", action="store_true",
        help="Write the archive file but skip the git commit + push step.")
    parser.add_argument("--only", choices=("daily", "random"),
        help="Only hit one endpoint instead of both.")
    args = parser.parse_args(argv)

    today = time.strftime("%Y-%m-%d")
    push = not args.no_push

    kinds = (args.only,) if args.only else ("daily", "random")
    rc = 0
    for k in kinds:
        if fetch_one(k, today, push) != 0:
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
