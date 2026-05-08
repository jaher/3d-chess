"""
Build openings/openings.tsv from the lichess chess-openings TSVs.

Each line of the source TSVs is `eco \t name \t pgn`. We replay the
PGN, take the FEN of the resulting position, and emit a single
combined `position-fen \t name` table — keyed on the four-field
FEN (board / turn / castling / ep target) so transpositions land on
the same row regardless of the move count that produced them.

Run after fetching fresh source TSVs:

    cd /tmp && for ch in a b c d e; do
      curl -sL -o openings_$ch.tsv \
        "https://raw.githubusercontent.com/lichess-org/chess-openings/master/$ch.tsv"
    done
    python3 tools/build_openings.py
"""

import csv
import os
import sys

import chess
import chess.pgn

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = "/tmp"
OUT_PATH = os.path.join(ROOT, "openings", "openings.tsv")


def position_fen(board: chess.Board) -> str:
    # Drop halfmove + fullmove counters so transpositions match on the
    # static position.
    parts = board.fen().split()
    return " ".join(parts[:4])


def parse_row(eco: str, name: str, pgn: str):
    board = chess.Board()
    # The TSV PGN is a bare move list ("1. e4 e5 2. Nf3 Nc6"). Tokenise
    # cheaply: drop move numbers + dots, walk SANs.
    tokens = []
    for tok in pgn.split():
        if tok.endswith("."):
            continue
        if "." in tok and tok.split(".")[0].isdigit():
            tail = tok.split(".", 1)[1]
            if tail:
                tokens.append(tail)
            continue
        tokens.append(tok)
    for san in tokens:
        try:
            board.push_san(san)
        except ValueError:
            return None
    return position_fen(board), eco, name


def main():
    rows = []
    seen = {}
    for ch in ("a", "b", "c", "d", "e"):
        src = os.path.join(SRC_DIR, f"openings_{ch}.tsv")
        if not os.path.exists(src):
            print(f"missing {src}", file=sys.stderr)
            sys.exit(1)
        with open(src, newline="") as fh:
            reader = csv.reader(fh, delimiter="\t")
            header = next(reader)
            assert header[0] == "eco" and header[1] == "name" and header[2] == "pgn", header
            for eco, name, pgn in reader:
                parsed = parse_row(eco, name, pgn)
                if parsed is None:
                    continue
                fen, eco_out, name_out = parsed
                # Keep the longer/more-specific name for any FEN that
                # multiple rows resolve to. Lichess's data lists root
                # opening + sub-variations as separate rows; the
                # variation rows generally have more text and are more
                # interesting to announce.
                if fen in seen:
                    if len(name_out) > len(seen[fen][1]):
                        seen[fen] = (eco_out, name_out)
                else:
                    seen[fen] = (eco_out, name_out)

    out_rows = sorted(seen.items())
    os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
    with open(OUT_PATH, "w") as fh:
        fh.write("fen\teco\tname\n")
        for fen, (eco, name) in out_rows:
            fh.write(f"{fen}\t{eco}\t{name}\n")
    print(f"wrote {OUT_PATH}: {len(out_rows)} rows")


if __name__ == "__main__":
    main()
