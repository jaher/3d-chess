#!/usr/bin/env python3
# Minimal UCI-speaking process used by tests/engine_test.cpp to exercise
# the Stockfish subprocess wrapper in ai_player.cpp without depending
# on a real Stockfish build. Logs every command to FAKE_STOCKFISH_LOG
# (truncated on each spawn) and reads the multi-line response for "go"
# commands from FAKE_STOCKFISH_RESPONSE if present, falling back to a
# trivial "bestmove e2e4" otherwise.

import os
import sys

LOG_FILE = os.environ.get("FAKE_STOCKFISH_LOG", "/tmp/fake_stockfish.log")
RESPONSE_FILE = os.environ.get("FAKE_STOCKFISH_RESPONSE",
                               "/tmp/fake_stockfish.response")

open(LOG_FILE, "w").close()

while True:
    line = sys.stdin.readline()
    if not line:
        break
    line = line.rstrip("\r\n")
    with open(LOG_FILE, "a") as f:
        f.write(line + "\n")

    if line == "uci":
        print("id name FakeFish")
        print("id author test")
        print("uciok", flush=True)
    elif line == "isready":
        print("readyok", flush=True)
    elif line == "quit":
        break
    elif line.startswith("go"):
        if os.path.exists(RESPONSE_FILE):
            with open(RESPONSE_FILE) as f:
                sys.stdout.write(f.read())
        else:
            print("info depth 1 score cp 0")
            print("bestmove e2e4")
        sys.stdout.flush()
    # ucinewgame, setoption, position: silent (no UCI response required)
