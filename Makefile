CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra

# OS detection. On macOS, prepend the Homebrew pkgconfig dir so we find
# gtk+-3.0 / epoxy installed via `brew install gtk+3 libepoxy`.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
    ifneq ($(BREW_PREFIX),)
        PKG_CONFIG := PKG_CONFIG_PATH=$(BREW_PREFIX)/lib/pkgconfig:$(BREW_PREFIX)/opt/libffi/lib/pkgconfig pkg-config
    else
        PKG_CONFIG := pkg-config
    endif
else
    PKG_CONFIG := pkg-config
endif

CXXFLAGS += $(shell $(PKG_CONFIG) --cflags gtk+-3.0 epoxy sdl2)
LDFLAGS  := $(shell $(PKG_CONFIG) --libs gtk+-3.0 epoxy sdl2) -lz -lm

TARGET   := chess
SRCS     := main.cpp chess_types.cpp chess_rules.cpp game_state.cpp app_state.cpp \
            board_renderer.cpp challenge.cpp cloth_flag.cpp linalg.cpp \
            stl_model.cpp shader.cpp ai_player.cpp time_control.cpp audio.cpp \
            compression.cpp menu_physics.cpp menu_input.cpp \
            challenge_ui.cpp
OBJS     := $(SRCS:.cpp=.o)
HEADERS  := chess_types.h chess_rules.h game_state.h app_state.h board_renderer.h \
            challenge.h cloth_flag.h linalg.h shader.h stl_model.h ai_player.h \
            time_control.h audio.h compression.h menu_physics.h menu_input.h \
            challenge_ui.h render_internal.h

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
	-$(MAKE) -C tests clean

distclean: clean
	-$(MAKE) -C $(STOCKFISH_DIR)/src clean

# Build and run the unit-test binary (see tests/). No GL, no GTK, no
# Stockfish subprocess — just the pure-logic layer.
test:
	$(MAKE) -C tests test

.PHONY: all clean distclean test
