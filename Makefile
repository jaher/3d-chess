CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra
CXXFLAGS += $(shell pkg-config --cflags gtk+-3.0 epoxy)
LDFLAGS  := $(shell pkg-config --libs gtk+-3.0 epoxy) -lm

TARGET   := chess
SRCS     := main.cpp chess_types.cpp chess_rules.cpp game_state.cpp board_renderer.cpp \
            challenge.cpp linalg.cpp stl_model.cpp shader.cpp ai_player.cpp
OBJS     := $(SRCS:.cpp=.o)
HEADERS  := chess_types.h chess_rules.h game_state.h board_renderer.h challenge.h \
            linalg.h shader.h stl_model.h ai_player.h

STOCKFISH_DIR := third_party/stockfish
STOCKFISH_BIN := $(STOCKFISH_DIR)/src/stockfish

all: $(TARGET) $(STOCKFISH_BIN)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(STOCKFISH_BIN):
	@if [ ! -f $(STOCKFISH_DIR)/src/Makefile ]; then \
		echo "Stockfish submodule not initialized."; \
		echo "Run: git submodule update --init --recursive"; \
		exit 1; \
	fi
	$(MAKE) -C $(STOCKFISH_DIR)/src -j build

clean:
	rm -f $(OBJS) $(TARGET)

distclean: clean
	-$(MAKE) -C $(STOCKFISH_DIR)/src clean

.PHONY: all clean distclean
