CXX      := g++
CXXFLAGS := -std=c++17 -O0 -g -Wall -Wextra -I.. -DAI_PLAYER_HELPERS_ONLY

# Shared source files from the parent directory. Only the pure-logic
# layer — no GTK, no GL, no SDL, no Stockfish subprocess. The
# AI_PLAYER_HELPERS_ONLY macro strips the POSIX subprocess wrapper
# from ai_player.cpp, exactly as the web build does.
SHARED_SRCS := \
    ../chess_types.cpp \
    ../chess_rules.cpp \
    ../challenge.cpp \
    ../linalg.cpp \
    ../ai_player.cpp

TEST_SRCS := \
    doctest_main.cpp \
    chess_rules_test.cpp \
    ai_player_helpers_test.cpp \
    challenge_test.cpp \
    linalg_test.cpp

HEADERS := doctest.h helpers.h

run_tests: $(SHARED_SRCS) $(TEST_SRCS) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(SHARED_SRCS) $(TEST_SRCS) -o $@ -lm

# Default target: build + run.
test: run_tests
	./run_tests

clean:
	rm -f run_tests

.PHONY: test clean
