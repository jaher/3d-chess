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
            voice_input.cpp voice_whisper.cpp \
            voice_tts.cpp voice_tts_native.cpp \
            chessnut_bridge.cpp phantom_bridge.cpp
OBJS     := $(SRCS:.cpp=.o)
HEADERS  := chess_types.h chess_rules.h game_state.h app_state.h board_renderer.h \
            challenge.h cloth_flag.h vec.h mat.h shader.h stl_model.h \
            ai_player.h time_control.h audio.h compression.h menu_physics.h \
            menu_input.h challenge_ui.h pregame_ui.h shatter_transition.h \
            render_internal.h text_atlas.h options_ui.h \
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

# espeak-ng for the voice-output (TTS) move announcer. Built via its
# own CMake into a static lib; voice data ships with the source tree
# and is loaded at runtime via the absolute path baked in at compile
# time. Disabling all the optional deps (sonic / pcaudio / mbrola /
# async) keeps the static lib lean and avoids a runtime ALSA/PulseAudio
# dependency — TTS PCM samples come back via the synth callback and
# are mixed into our existing SDL2 audio stream.
ESPEAK_DIR    := third_party/espeak-ng
ESPEAK_BUILD  := $(ESPEAK_DIR)/build
ESPEAK_LIB    := $(ESPEAK_BUILD)/src/libespeak-ng/libespeak-ng.a
# espeak-ng's CMake build also produces two helper static libs the
# main lib has unresolved references into: libucd (Unicode database
# used by readclause / dictionary / numbers) and libspeechPlayer
# (Klatt formant synthesiser). Both are private deps so cmake only
# emits them as separate archives — link them explicitly.
ESPEAK_AUX_LIBS := $(ESPEAK_BUILD)/src/ucd-tools/libucd.a \
                   $(ESPEAK_BUILD)/src/speechPlayer/libspeechPlayer.a
ESPEAK_INC    := $(ESPEAK_DIR)/src/include
# espeak-ng's runtime data (phontab / phondata / phonindex /
# intonations + per-language *_dict files) is *generated* by the
# CMake build, not committed to the source tree. Point at the
# build dir's espeak-ng-data/ where the `data` target writes.
ESPEAK_DATA   := $(abspath $(ESPEAK_BUILD))
ESPEAK_DATA_MARKER := $(ESPEAK_BUILD)/espeak-ng-data/phontab
ESPEAK_CMAKE_ARGS := -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
                     -DUSE_ASYNC=OFF -DUSE_MBROLA=OFF -DUSE_LIBSONIC=OFF \
                     -DUSE_LIBPCAUDIO=OFF -DUSE_KLATT=ON \
                     -DUSE_SPEECHPLAYER=ON \
                     -DCMAKE_POSITION_INDEPENDENT_CODE=ON
CXXFLAGS += -I$(ESPEAK_INC) -DESPEAK_DATA_PATH='"$(ESPEAK_DATA)"'
LDFLAGS  += $(ESPEAK_LIB) $(ESPEAK_AUX_LIBS)

# Pre-converted distil-small.en GGML, hosted by the whisper.cpp/HF
# community. Pinning by sha256 keeps the build reproducible — replace
# WHISPER_MODEL_URL / WHISPER_MODEL_SHA256 if the upstream link rotates.
WHISPER_MODEL_URL    := https://huggingface.co/distil-whisper/distil-small.en/resolve/main/ggml-distil-small.en.bin
WHISPER_MODEL_SHA256 := ?

all: $(TARGET) $(STOCKFISH_BIN) $(WHISPER_MODEL)

$(TARGET): $(OBJS) $(WHISPER_LIBS) $(SIMPLEBLE_LIB) $(ESPEAK_LIB) $(ESPEAK_DATA_MARKER)
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

# espeak-ng — built via its own CMake. The lib's voice data lives at
# $(ESPEAK_DATA)/espeak-ng-data/ and is referenced at runtime via the
# ESPEAK_DATA_PATH compile-time define. Build skips the language
# data generation and CLI executable to keep build time short.
$(ESPEAK_LIB):
	@if [ ! -f $(ESPEAK_DIR)/CMakeLists.txt ]; then \
		echo "espeak-ng submodule not initialized."; \
		echo "Run: git submodule update --init --recursive"; \
		exit 1; \
	fi
	cmake -B $(ESPEAK_BUILD) -S $(ESPEAK_DIR) $(ESPEAK_CMAKE_ARGS)
	cmake --build $(ESPEAK_BUILD) --config Release -j --target espeak-ng

# Generate the phoneme tables (phontab / phondata / phonindex /
# intonations) and per-language *_dict files into
# $(ESPEAK_BUILD)/espeak-ng-data/. espeak_Initialize at runtime
# refuses to start without phontab (the user reported "No such
# file or directory" right after a move because we'd previously
# only built the static lib). The CMake `data` target writes them.
$(ESPEAK_DATA_MARKER): $(ESPEAK_LIB)
	cmake --build $(ESPEAK_BUILD) --config Release -j --target data

clean:
	rm -f $(OBJS) $(TARGET)
	-$(MAKE) -C tests clean

distclean: clean
	-$(MAKE) -C $(STOCKFISH_DIR)/src clean
	-rm -rf $(WHISPER_BUILD) $(SIMPLEBLE_BUILD) $(ESPEAK_BUILD)

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

.PHONY: all clean distclean test fetch-whisper-model simpleble_scan
