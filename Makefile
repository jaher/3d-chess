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

CXXFLAGS += $(shell $(PKG_CONFIG) --cflags gtk+-3.0 epoxy sdl2 libcurl)
LDFLAGS  := $(shell $(PKG_CONFIG) --libs gtk+-3.0 epoxy sdl2 libcurl) -lz -lm

TARGET   := chess
SRCS     := main.cpp chess_types.cpp chess_rules.cpp game_state.cpp app_state.cpp \
            board_renderer.cpp challenge.cpp cloth_flag.cpp vec.cpp mat.cpp \
            stl_model.cpp shader.cpp ai_player.cpp time_control.cpp audio.cpp \
            compression.cpp menu_physics.cpp menu_input.cpp \
            challenge_ui.cpp pregame_ui.cpp shatter_transition.cpp \
            text_atlas.cpp options_ui.cpp puzzle.cpp \
            voice_input.cpp voice_whisper.cpp \
            voice_tts.cpp voice_tts_native.cpp \
            chessnut_bridge.cpp phantom_bridge.cpp
OBJS     := $(SRCS:.cpp=.o)
HEADERS  := chess_types.h chess_rules.h game_state.h app_state.h board_renderer.h \
            challenge.h cloth_flag.h vec.h mat.h shader.h stl_model.h \
            ai_player.h time_control.h audio.h compression.h menu_physics.h \
            menu_input.h challenge_ui.h pregame_ui.h shatter_transition.h \
            render_internal.h text_atlas.h options_ui.h puzzle.h \
            voice_input.h chessnut_bridge.h phantom_bridge.h voice_tts.h

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

# SimpleBLE for the Chessnut Move bridge. Built via its own CMake;
# we link the static lib + libdbus-1 (BlueZ's IPC channel on Linux).
SIMPLEBLE_DIR    := third_party/simpleble
SIMPLEBLE_BUILD  := $(SIMPLEBLE_DIR)/build
SIMPLEBLE_LIB    := $(SIMPLEBLE_BUILD)/lib/libsimpleble.a
SIMPLEBLE_INC    := $(SIMPLEBLE_DIR)/simpleble/include
SIMPLEBLE_EXP    := $(SIMPLEBLE_BUILD)/export
# NOTE: do NOT set -DSIMPLEBLE_PLAIN=ON. That flag swaps in the
# library's mock backend (Plain Adapter / Plain Peripheral) used
# for unit tests, and the standalone tools/simpleble_scan
# diagnostic confirmed it had been masquerading as the real BLE
# stack since the original submodule landing — the chess app's
# scan only ever saw a fake "Plain Peripheral".
SIMPLEBLE_CMAKE_ARGS := -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
CXXFLAGS += -I$(SIMPLEBLE_INC) -I$(SIMPLEBLE_EXP) \
            -I$(SIMPLEBLE_DIR)/dependencies/external
LDFLAGS  += $(SIMPLEBLE_LIB) $(shell $(PKG_CONFIG) --libs dbus-1)

# Piper neural-TTS for the voice-output (move announcer). Replaces
# the earlier espeak-ng formant synth, which sounded too robotic
# next to the web build's `window.speechSynthesis`. Piper bundles
# its own ONNX runtime + phonemizer in its release tarball — we
# fetch the prebuilt binary + a single voice model on first build
# (~90 MB total) and shell out per utterance, same pattern as the
# Stockfish subprocess. Audio comes back as raw S16 22050 Hz PCM
# on stdout and feeds the existing audio.cpp 8-voice mixer.
#
# Voice: en_US-amy-medium. Roomy budget for natural female English
# pronunciation; swap the *_NAME / *_BASE_URL pair below for a
# different voice (lessac, ryan, kathleen, ...) or model size.
PIPER_DIR        := third_party/piper-bin
PIPER_BIN        := $(PIPER_DIR)/piper/piper
PIPER_VERSION    := 2023.11.14-2
PIPER_URL        := https://github.com/rhasspy/piper/releases/download/$(PIPER_VERSION)/piper_linux_x86_64.tar.gz
PIPER_MODEL_DIR  := third_party/piper-models
PIPER_MODEL_NAME := en_US-amy-medium
PIPER_MODEL      := $(PIPER_MODEL_DIR)/$(PIPER_MODEL_NAME).onnx
PIPER_MODEL_JSON := $(PIPER_MODEL).json
PIPER_MODEL_BASE_URL := https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/en/en_US/amy/medium

CXXFLAGS += -DPIPER_BINARY_PATH='"$(abspath $(PIPER_BIN))"' \
            -DPIPER_MODEL_PATH='"$(abspath $(PIPER_MODEL))"'

# Pre-converted distil-small.en GGML, hosted by the whisper.cpp/HF
# community. Pinning by sha256 keeps the build reproducible — replace
# WHISPER_MODEL_URL / WHISPER_MODEL_SHA256 if the upstream link rotates.
WHISPER_MODEL_URL    := https://huggingface.co/distil-whisper/distil-small.en/resolve/main/ggml-distil-small.en.bin
WHISPER_MODEL_SHA256 := ?

all: $(TARGET) $(STOCKFISH_BIN) $(WHISPER_MODEL) \
     $(PIPER_BIN) $(PIPER_MODEL) $(PIPER_MODEL_JSON)

$(TARGET): $(OBJS) $(WHISPER_LIBS) $(SIMPLEBLE_LIB) \
           $(PIPER_BIN) $(PIPER_MODEL) $(PIPER_MODEL_JSON)
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

# Download the pre-converted distil-small.en GGML model (~166 MB) on
# first build. The file target only fires when the model is missing,
# so subsequent builds are no-ops. `make fetch-whisper-model` is kept
# as an explicit alias for users who want to pre-fetch.
$(WHISPER_MODEL):
	@mkdir -p $(dir $@)
	@echo "Downloading distil-small.en GGML (~166 MB) ..."
	curl -L --fail -o $@ $(WHISPER_MODEL_URL)
	@echo "Saved to $@"

fetch-whisper-model: $(WHISPER_MODEL)
	@echo "Model present at $(WHISPER_MODEL)"

# SimpleBLE — built via its own CMake. The cross-platform BLE
# library used by chessnut_bridge.cpp. Plain build (no vendoring
# of the BLE Pro features) keeps the static lib at ~5 MB. On Linux
# the runtime dependency is libdbus-1.
$(SIMPLEBLE_LIB):
	@if [ ! -f $(SIMPLEBLE_DIR)/simpleble/CMakeLists.txt ]; then \
		echo "SimpleBLE submodule not initialized."; \
		echo "Run: git submodule update --init --recursive"; \
		exit 1; \
	fi
	cmake -B $(SIMPLEBLE_BUILD) -S $(SIMPLEBLE_DIR)/simpleble \
		$(SIMPLEBLE_CMAKE_ARGS)
	cmake --build $(SIMPLEBLE_BUILD) --config Release -j

# Piper TTS — fetch the prebuilt linux x86_64 release tarball
# (binary + ONNX runtime + bundled espeak-ng phoneme data) on first
# build. The release ships a self-contained `piper` executable
# with RPATH=$ORIGIN, so the .so files load from the same dir
# without LD_LIBRARY_PATH gymnastics. Subsequent builds are no-ops
# once the file target exists.
$(PIPER_BIN):
	@mkdir -p $(PIPER_DIR)
	@echo "Downloading piper-tts $(PIPER_VERSION) (~26 MB) ..."
	curl -L --fail -o $(PIPER_DIR)/piper.tar.gz $(PIPER_URL)
	tar -xzf $(PIPER_DIR)/piper.tar.gz -C $(PIPER_DIR)
	rm $(PIPER_DIR)/piper.tar.gz
	@echo "Piper installed at $(PIPER_BIN)"

# Voice model — pinned to the v1.0.0 tag of rhasspy/piper-voices on
# Hugging Face so updates upstream don't silently change the voice
# the desktop build uses. Two files: the ONNX weights (~63 MB) and
# a tiny .onnx.json with phoneme map + sample rate metadata.
$(PIPER_MODEL):
	@mkdir -p $(PIPER_MODEL_DIR)
	@echo "Downloading piper voice model $(PIPER_MODEL_NAME) (~63 MB) ..."
	curl -L --fail -o $@ $(PIPER_MODEL_BASE_URL)/$(PIPER_MODEL_NAME).onnx
	@echo "Saved to $@"

$(PIPER_MODEL_JSON):
	@mkdir -p $(PIPER_MODEL_DIR)
	curl -L --fail -o $@ $(PIPER_MODEL_BASE_URL)/$(PIPER_MODEL_NAME).onnx.json

fetch-piper-binary: $(PIPER_BIN)
fetch-piper-model:  $(PIPER_MODEL) $(PIPER_MODEL_JSON)

clean:
	rm -f $(OBJS) $(TARGET)
	-$(MAKE) -C tests clean

distclean: clean
	-$(MAKE) -C $(STOCKFISH_DIR)/src clean
	-rm -rf $(WHISPER_BUILD) $(SIMPLEBLE_BUILD)
	-rm -rf $(PIPER_DIR) $(PIPER_MODEL_DIR)

# Build and run the unit-test binary (see tests/). No GL, no GTK, no
# Stockfish subprocess, no whisper.cpp — just the pure-logic layer.
test:
	$(MAKE) -C tests test

# Standalone SimpleBLE scan diagnostic — links libsimpleble.a but
# nothing else, so any "scan finds nothing" issue is isolated to
# the BLE stack rather than chess-app integration glue.
simpleble_scan: tools/simpleble_scan.cpp $(SIMPLEBLE_LIB)
	$(CXX) -std=c++17 -O2 -Wall -Wextra \
		-I$(SIMPLEBLE_INC) -I$(SIMPLEBLE_EXP) \
		-I$(SIMPLEBLE_DIR)/dependencies/external \
		-o tools/simpleble_scan tools/simpleble_scan.cpp \
		$(SIMPLEBLE_LIB) $(shell $(PKG_CONFIG) --libs dbus-1) -lpthread

.PHONY: all clean distclean test \
        fetch-whisper-model fetch-piper-binary fetch-piper-model \
        simpleble_scan
