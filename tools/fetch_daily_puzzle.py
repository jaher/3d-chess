#!/usr/bin/env python3
"""
Fetch the chess.com Puzzle of the Day and archive it to ../puzzles/.

Designed to be run from cron every few hours (the daily puzzle
rotates around 00:00 UTC, so 12 h cadence catches it within ~12 h
of publication). Dedup is by FEN: if any existing file in
puzzles/*.md already contains today's FEN as a literal line, we
skip the write — same puzzle reposted, or this script firing
twice within a single day.

Format of the written file mirrors challenges/*.md so the same
parser shape applies (name + type + side + FEN, with the
chess.com solution PGN appended as a comment block).

When a new file IS written, the script also commits it as its own
single-file commit and pushes to the configured origin via the
local `git` binary. Authentication is whatever git is already
configured to use on this machine (SSH key, credential helper,
or the URL stored in `.git/config`). The script never reads or
embeds any secret of its own — no environment variables are
inspected, no tokens hardcoded, no stdout / stderr from git is
re-emitted beyond a short summary line. Pass --no-push to skip
the commit + push step (useful for local testing or for cloning
the repo somewhere read-only).

Suggested crontab line — run twice a day:
    0 */12 * * * /path/to/3d_chess/tools/fetch_daily_puzzle.py >> /tmp/fetch_daily_puzzle.log 2>&1
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
ENDPOINT = "https://api.chess.com/pub/puzzle"
USER_AGENT = "3d-chess fetch_daily_puzzle.py (cron)"
TIMEOUT_S = 15


def log(msg: str) -> None:
    """Timestamp + write to stderr so cron's stderr-on-failure mail surfaces it."""
    ts = time.strftime("%Y-%m-%dT%H:%M:%S")
    print(f"[{ts}] {msg}", file=sys.stderr)


def fetch_puzzle() -> dict:
    req = urllib.request.Request(ENDPOINT, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=TIMEOUT_S) as r:
        if r.status != 200:
            raise RuntimeError(f"HTTP {r.status} from {ENDPOINT}")
        body = r.read().decode("utf-8", errors="replace")
    return json.loads(body)


def fen_already_archived(fen: str) -> bool:
    """True if any *.md in puzzles/ contains the FEN as a standalone line.

    The in-app archiver writes the FEN on its own line, so a literal-
    string match against any line is enough to catch dupes regardless
    of which slug variant the file was saved under."""
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
    """URL-tail to filename slug. Drop everything that isn't safe in a path."""
    return re.sub(r"[^A-Za-z0-9._-]+", "_", s).strip("_")


def side_from_fen(fen: str) -> str:
    parts = fen.split()
    return "black" if len(parts) >= 2 and parts[1] == "b" else "white"


def archive_path_for(today: str, url: str) -> str:
    """Pick a non-clobbering path under PUZZLES_DIR/."""
    slug = slugify(os.path.basename(url)) if url else ""
    # If the slug duplicates the date prefix (chess.com daily URLs
    # are .../daily/YYYY-MM-DD), drop it so the filename reads as
    # plain "YYYY-MM-DD.md".
    if slug == today:
        slug = ""
    base = today if not slug else f"{today}_{slug}"
    path = os.path.join(PUZZLES_DIR, base + ".md")
    suffix = 2
    while os.path.exists(path):
        path = os.path.join(PUZZLES_DIR, f"{base}_v{suffix}.md")
        suffix += 1
    return path


def write_archive(p: dict, today: str) -> str:
    fen = p["fen"]
    title = p.get("title") or ""
    url = p.get("url") or ""
    pgn = p.get("pgn") or ""

    os.makedirs(PUZZLES_DIR, exist_ok=True)
    path = archive_path_for(today, url)

    lines: list[str] = []
    lines.append("# Chess.com Daily Puzzle archive")
    lines.append("#")
    lines.append(f"# Fetched on {today} by tools/fetch_daily_puzzle.py")
    lines.append("# Format mirrors challenges/*.md so this file is greppable")
    lines.append("# / replayable as a single-page challenge.")
    if title:
        lines.append(f"# title: {title}")
    if url:
        lines.append(f"# url:   {url}")
    lines.append("")
    if title:
        lines.append(f"name: {title}")
        lines.append("")
    lines.append("type: puzzle")
    lines.append(f"side: {side_from_fen(fen)}")
    lines.append(fen)
    if pgn:
        lines.append("")
        lines.append("# --- Solution PGN (chess.com) ---")
        for pgn_line in pgn.splitlines():
            lines.append(f"# {pgn_line}".rstrip())

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


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--no-push", action="store_true",
        help="Write the archive file but skip the git commit + push step.")
    args = parser.parse_args(argv)

    try:
        p = fetch_puzzle()
    except (urllib.error.URLError, OSError, json.JSONDecodeError) as e:
        log(f"fetch failed: {e}")
        return 1

    fen = (p.get("fen") or "").strip()
    if not fen:
        log("response had no FEN; skipping")
        return 1

    if fen_already_archived(fen):
        log(f"already archived (FEN match) — skip: {fen}")
        return 0

    today = time.strftime("%Y-%m-%d")
    path = write_archive(p, today)
    log(f"wrote {os.path.relpath(path, PROJECT_ROOT)}")
    if not args.no_push:
        git_commit_and_push(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
