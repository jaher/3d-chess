# Opening drills for the trainer. Each block is a "name:" then a "line:"
# of UCI moves. You play the side that moves first (White in all of
# these); the app auto-plays the book replies, and a wrong move rewinds
# to the start so you can try the line again.
#
# Lines end on your move so finishing the line = drill solved. Every
# line is checked legal move-by-move at build time by
# tests/openings_drills_test.cpp.

name: Italian Game
line: e2e4 e7e5 g1f3 b8c6 f1c4

name: Ruy Lopez
line: e2e4 e7e5 g1f3 b8c6 f1b5 a7a6 b5a4 g8f6 e1g1

name: Vienna Game
line: e2e4 e7e5 b1c3 g8f6 f2f4

name: Queen's Gambit
line: d2d4 d7d5 c2c4 e7e6 b1c3 g8f6 c1g5

name: London System
line: d2d4 d7d5 g1f3 g8f6 c1f4 e7e6 e2e3

name: Sicilian, Open
line: e2e4 c7c5 g1f3 d7d6 d2d4 c5d4 f3d4 g8f6 b1c3
