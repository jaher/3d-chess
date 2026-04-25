# Multi-page chess challenge
#
# Each non-comment, non-empty line below is a FEN position.
# Every page declares its own `type:` and `side:` metadata so a
# single file can mix tactics types.
#
# Recognised types:
#   mate_in_1 / mate_in_2 / mate_in_3 — deliver checkmate in N moves.
#   find_forks                        — play a move that forks two+
#                                       enemy pieces.
#   find_pins                         — play a move that pins an enemy
#                                       piece against a more valuable one.
#
# Comments start with '#'. Blank lines are ignored.

name: SV Chess Thinkers Spring 2026 - Homework 2

# Page 1

# Top Left
type: find_forks
side: white
8/1b6/p6k/4p1p1/4P1Pp/3P1R1P/1b4K1/8 w - - 0 1

# Top Middle
type: find_forks
side: white
1k6/ppq4p/5r2/4Q3/8/4NP2/P5PP/7K w - - 0 1

# Top Right
type: find_forks
side: white
r2qk2r/ppp2ppp/2n1b3/2b5/2BpP3/8/PPPN1PPP/R1BQ1RK1 w - - 0 1

# Bottom Left
type: find_forks
side: white
8/p2br2k/1p4pp/2pq4/6N1/1P3P2/P3Q1PP/5RK1 w - - 0 1

# Bottom Middle
type: find_forks
side: white
r2q1rk1/ppp2ppp/2npb3/4n3/4P3/2NP4/PPP1B1PP/R1BQ1RK1 w - - 0 1

# Bottom Right
type: find_forks
side: white
r2qk2r/ppp2ppp/2np4/8/3Pn3/2P1B3/PP2NPPP/R2QK2R w - - 0 1

# Page 2

# Top Left
type: find_pins
side: white
Q7/6pk/3r3p/pP3p2/3R4/q6P/5PPK/8 w - - 0 1

# Top Middle
type: find_pins
side: white
5r1k/ppq1n2p/2n3p1/5p2/8/P3B3/QP3NPP/2R3K1 w - - 0 1

# Top Right
type: find_pins
side: white
r1bk3r/pppn1pbp/5np1/4p1B1/2P1P3/2N5/PP3PPP/2KR1BNR w - - 0 1

# Bottom Left
type: find_pins
side: white
4k3/p7/1p3q2/7p/2PR4/6P1/4P1K1/3R4 w - - 0 1

# Bottom Middle
type: find_pins
side: white
5kr1/4b2p/4pp2/1p1q4/pB6/1PQ4P/2P2PP1/1K5R w - - 0 1

# Bottom Right
type: find_pins
side: white
8/8/8/6q1/P6k/8/7K/1R6 w - - 0 1
