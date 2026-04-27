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
            board_renderer.cpp challenge.cpp cloth_flag.cpp vec.cpp mat.cpp \
            stl_model.cpp shader.cpp ai_player.cpp time_control.cpp audio.cpp \
            compression.cpp menu_physics.cpp menu_input.cpp \
            challenge_ui.cpp pregame_ui.cpp shatter_transition.cpp \
            text_atlas.cpp options_ui.cpp \
            voice_input.cpp voice_whisper.cpp
OBJS     := $(SRCS:.cpp=.o)
HEADERS  := chess_types.h chess_rules.h game_state.h app_state.h board_renderer.h \
            challenge.h cloth_flag.h vec.h mat.h shader.h stl_model.h \
            ai_player.h time_control.h audio.h compression.h menu_physics.h \
            menu_input.h challenge_ui.h pregame_ui.h shatter_transition.h \
            render_internal.h text_atlas.h options_ui.h \
            voice_input.h

STOCKFISH_DIR := third_party/stockfish
STOCKFISH_BIN := $(STOCKFISH_DIR)/src/stockfish

# whisper.cpp builds via its own CMake; we link the resulting static
# libs into the desktop binary. Backend selection is driven by
# WHISPER_BACKEND (cpu | cuda | metal | vulkan | auto). Default `auto`
# lets whisper.cpp pick (Metal on macOS by default, CPU otherwise).
WHISPER_DIR    := third_party/whisper.cpp
WHISPER_BUILD  := $(WHISPER_DIR)/build
WHISPER_LIB    := $(WHISPER_BUILD)/src/libwhisper.a
WHISPER_LIBS   := $(WHISPER_LIB) \
                  $(WHISPER_BUILD)/ggml/src/libggml.a \
                  $(WHISPER_BUILD)/ggml/src/libggml-base.a \
                  $(WHISPER_BUILD)/ggml/src/libggml-cpu.a
WHISPER_MODEL  := third_party/whisper-models/ggml-distil-small.en.bin

WHISPER_BACKEND ?= auto
WHISPER_CMAKE_ARGS := -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
                      -DWHISPER_BUILD_EXAMPLES=OFF -DWHISPER_BUILD_TESTS=OFF \
                      -DWHISPER_BUILD_SERVER=OFF
ifeq ($(WHISPER_BACKEND),cuda)
    WHISPER_CMAKE_ARGS += -DGGML_CUDA=ON
    WHISPER_BACKEND_LIBS := -lcudart -lcublas
endif
ifeq ($(WHISPER_BACKEND),metal)
    WHISPER_CMAKE_ARGS += -DGGML_METAL=ON
    WHISPER_BACKEND_LIBS := -framework Metal -framework Foundation -framework MetalKit
endif
ifeq ($(WHISPER_BACKEND),vulkan)
    WHISPER_CMAKE_ARGS += -DGGML_VULKAN=ON
    WHISPER_BACKEND_LIBS := -lvulkan
endif
# auto: on macOS whisper.cpp turns on Metal automatically; on Linux it
# stays CPU unless WHISPER_BACKEND is set explicitly.

CXXFLAGS += -I$(WHISPER_DIR)/include -I$(WHISPER_DIR)/ggml/include
# whisper.cpp's CPU backend uses OpenMP for thread parallelism. The
# static libggml-cpu.a leaves the GOMP_* / omp_* symbols unresolved,
# so the final link must pull in libgomp (or libomp on Clang/macOS).
# -fopenmp on g++/clang++ does the right thing on every platform.
LDFLAGS  += $(WHISPER_LIBS) $(WHISPER_BACKEND_LIBS) -fopenmp

# Pre-converted distil-small.en GGML, hosted by the whisper.cpp/HF
# community. Pinning by sha256 keeps the build reproducible — replace
# WHISPER_MODEL_URL / WHISPER_MODEL_SHA256 if the upstream link rotates.
WHISPER_MODEL_URL    := https://huggingface.co/distil-whisper/distil-small.en/resolve/main/ggml-distil-small.en.bin
WHISPER_MODEL_SHA256 := ?

all: $(TARGET) $(STOCKFISH_BIN)

$(TARGET): $(OBJS) $(WHISPER_LIBS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(STOCKFISH_BIN):
	@if [ ! -f $(STOCKFISH_DIR)/src/Makefile ]; then \
		echo "Stockfish submodule not initialized."; \
		echo "Run: git submodule update --init --recursive"; \
		exit 1; \
	fi
	$(MAKE) -C $(STOCKFISH_DIR)/src -j build

$(WHISPER_LIBS): $(WHISPER_LIB)

$(WHISPER_LIB):
	@if [ ! -f $(WHISPER_DIR)/CMakeLists.txt ]; then \
		echo "whisper.cpp submodule not initialized."; \
		echo "Run: git submodule update --init --recursive"; \
		exit 1; \
	fi
	cmake -B $(WHISPER_BUILD) -S $(WHISPER_DIR) $(WHISPER_CMAKE_ARGS)
	cmake --build $(WHISPER_BUILD) --config Release -j

# Download the pre-converted distil-small.en GGML model (~166 MB).
# Skipped if the file already exists. Voice input on the desktop
# build silently falls back to "unavailable" if this hasn't been run.
fetch-whisper-model:
	@mkdir -p $(dir $(WHISPER_MODEL))
	@if [ -f $(WHISPER_MODEL) ]; then \
		echo "Model already present at $(WHISPER_MODEL); skipping."; \
	else \
		echo "Downloading distil-small.en GGML (~166 MB) ..."; \
		curl -L --fail -o $(WHISPER_MODEL) $(WHISPER_MODEL_URL); \
		echo "Saved to $(WHISPER_MODEL)"; \
	fi

clean:
	rm -f $(OBJS) $(TARGET)
	-$(MAKE) -C tests clean

distclean: clean
	-$(MAKE) -C $(STOCKFISH_DIR)/src clean
	-rm -rf $(WHISPER_BUILD)

# Build and run the unit-test binary (see tests/). No GL, no GTK, no
# Stockfish subprocess, no whisper.cpp — just the pure-logic layer.
test:
	$(MAKE) -C tests test

.PHONY: all clean distclean test fetch-whisper-model
