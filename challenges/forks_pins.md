# Forks & pins drill for the Practice screen. Find a move that creates
# the motif (a fork hits two+ enemy pieces; a pin freezes one against a
# more valuable piece behind it). Up to three candidates per position.
#
# Every position is auto-verified by tests/training_content_test.cpp via
# the engine's own find_tactic_moves enumerator, so a legal motif always
# exists. See homework1.md for the full file format.

name: Forks & Pins

# Page 1 — forks

# Knight forks king and rook
type: find_forks
side: white
r3k3/8/8/3N4/8/8/8/6K1 w - - 0 1

# Pawn forks two knights
type: find_forks
side: white
8/8/2n1n3/8/3P4/8/8/k5K1 w - - 0 1

# Knight forks king and queen
type: find_forks
side: white
8/8/q3k3/8/4N3/8/8/6K1 w - - 0 1

# Knight forks two rooks
type: find_forks
side: white
3rkr2/8/8/6N1/8/8/8/6K1 w - - 0 1

# Page 2 — pins

# Bishop pins the knight to the queen
type: find_pins
side: white
3q2k1/8/5n2/8/8/8/8/2B3K1 w - - 0 1

# Rook pins the knight to the king
type: find_pins
side: white
4k3/8/8/4n3/8/8/8/R5K1 w - - 0 1

# Rook pins the knight to the king (rank shift)
type: find_pins
side: white
5k2/8/5n2/8/8/8/8/R5K1 w - - 0 1

# Bishop pins the knight to the king
type: find_pins
side: white
4k3/3n4/8/8/8/8/4B3/6K1 w - - 0 1
