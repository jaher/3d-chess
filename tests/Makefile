CXX      := g++
CXXFLAGS := -std=c++17 -O0 -g -Wall -Wextra -I..

# Pure-logic test binary (run_tests). The AI_PLAYER_HELPERS_ONLY macro
# strips the POSIX subprocess wrapper from ai_player.cpp, exactly as
# the web build does. No GTK, no GL, no SDL, no Stockfish subprocess.
PURE_FLAGS := $(CXXFLAGS) -DAI_PLAYER_HELPERS_ONLY

SHARED_SRCS := \
    ../chess_types.cpp \
    ../chess_rules.cpp \
    ../challenge.cpp \
    ../vec.cpp \
    ../mat.cpp \
    ../ai_player.cpp \
    ../voice_input.cpp \
    ../voice_tts.cpp \
    ../puzzle.cpp

TEST_SRCS := \
    doctest_main.cpp \
    chess_rules_test.cpp \
    ai_player_helpers_test.cpp \
    challenge_test.cpp \
    linalg_test.cpp \
    voice_input_test.cpp \
    voice_tts_test.cpp \
    puzzle_test.cpp

HEADERS := doctest.h helpers.h

run_tests: $(SHARED_SRCS) $(TEST_SRCS) $(HEADERS)
	$(CXX) $(PURE_FLAGS) $(SHARED_SRCS) $(TEST_SRCS) -o $@ -lm

# Engine test binary (run_tests_engine) — recompiles ai_player.cpp WITHOUT
# AI_PLAYER_HELPERS_ONLY so the Stockfish subprocess wrapper is linked in,
# and drives it via tests/fake_stockfish.py (no real Stockfish required).
ENGINE_TEST_SRCS := doctest_main.cpp engine_test.cpp

run_tests_engine: $(SHARED_SRCS) $(ENGINE_TEST_SRCS) $(HEADERS) fake_stockfish.py
	$(CXX) $(CXXFLAGS) $(SHARED_SRCS) $(ENGINE_TEST_SRCS) -o $@ -lm -lpthread

# Default target: build + run both binaries.
test: run_tests run_tests_engine
	./run_tests
	./run_tests_engine

# Skip the engine binary (e.g. for CI environments without python3).
test_pure: run_tests
	./run_tests

clean:
	rm -f run_tests run_tests_engine

.PHONY: test test_pure clean
