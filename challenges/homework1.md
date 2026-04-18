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

# Page 1

# Top Left
type: mate_in_2
side: white
5r1k/6p1/8/2q3N1/8/8/5PPP/3Q2K1 w - - 0 1

# Top Middle
type: mate_in_2
side: white
5rk1/b4p2/1q2p3/8/8/8/1B3PP1/2Q1R1K1 w - - 0 1

# Top Right
type: mate_in_2
side: white
k1r5/1p6/1pq5/1N6/8/1Q6/1PP5/1K6 w - - 0 1

# Bottom Left
type: mate_in_2
side: white
6k1/1pQ2qpp/n1p5/2Bp4/8/6Pb/2P2P1P/7K w - - 0 1

# Bottom Middle
type: mate_in_2
side: white
1r2q1k1/5p1p/2p5/p2n1N2/3p4/1P1P3P/P2Q2P1/2R4K w - - 0 1

# Bottom Right
type: mate_in_2
side: white
5k2/3q1ppp/4bB2/6Q1/8/6P1/5P1K/8 w - - 0 1

# Page 2

# Top Left
type: mate_in_2
side: white
4n1bk/4N3/6PK/8/8/8/8/8 w - - 0 1

# Top Middle
type: mate_in_2
side: white
4N2k/p5n1/2np2pp/4p3/4q3/8/6PP/5RBK w - - 0 1

# Top Right
type: mate_in_2
side: white
r2nr1k1/pp4pn/1q2p3/3p2N1/3P1N2/8/PP3PPP/RBB2RK1 w - - 0 1

# Bottom Left
type: mate_in_2
side: white
r1bq1k1r/pp1pRp1p/6p1/n2N4/2Bn4/5N2/PP3PPP/R1B3K1 w - - 0 1

# Bottom Middle
type: mate_in_2
side: white
2NN1nk1/6pp/8/8/3q4/2p5/2P5/R1K5 w - - 0 1

# Bottom Right
type: mate_in_2
side: white
4n2r/R5bk/8/6KN/8/8/8/8 w - - 0 1
