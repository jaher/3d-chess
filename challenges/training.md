# Mate-in-one warm-up — a quick tactics ladder for the Practice screen.
#
# Each non-comment, non-empty line is a FEN. Every page declares its
# own `type:` and `side:`. See homework1.md for the full format notes.
# These positions are auto-verified by tools/verify (mate_in_1 = a
# legal move that checkmates; find_forks / find_pins = a legal motif
# move exists), so they should never go stale.

name: Mate in One - Warm-up

# Page 1 — back-rank and ladder mates

# Back-rank rook mate
type: mate_in_1
side: white
6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1

# Rook ladder mate
type: mate_in_1
side: white
7k/1R6/8/8/8/8/8/R5K1 w - - 0 1

# Queen mate, bishop-supported
type: mate_in_1
side: white
7k/8/8/8/8/8/1B4Q1/6K1 w - - 0 1

# King-and-queen box mate
type: mate_in_1
side: white
6k1/4Q3/6K1/8/8/8/8/8 w - - 0 1

# Page 2 — queen finishes

# Queen back-rank mate
type: mate_in_1
side: white
7k/6pp/8/8/8/8/8/Q5K1 w - - 0 1

# Supported queen mate
type: mate_in_1
side: white
5k2/3Q4/5K2/8/8/8/8/8 w - - 0 1

# Page 3 — find the fork / pin

# Knight forks king and queen
type: find_forks
side: white
2k5/8/8/3N4/2q5/8/8/6K1 w - - 0 1

# Bishop pins the knight to the queen
type: find_pins
side: white
3q2k1/8/5n2/8/8/8/8/2B3K1 w - - 0 1
