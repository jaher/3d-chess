EMCC      := em++

# Short hash baked into the generated chess.html as the `?v=<HASH>`
# cache-bust suffix on chess.js / chess.wasm / chess.data /
# stockfish-bridge.js. Uses the current git HEAD (so CI deploys get
# the commit SHA), falling back to a wall-clock timestamp for dirty
# working trees or builds outside a git clone.
BUILD_HASH := $(shell git -C .. rev-parse --short HEAD 2>/dev/null || date +%s)
# USE_SDL=2 must be in both compile and link flags: it wires the SDL
# headers into the per-TU compile sysroot *and* pulls in the SDL lib
# at link time. Leaving it in LDFLAGS only worked with the previous
# monolithic em++ invocation (where compile and link were one step)
# but breaks the split per-TU build because the compile steps couldn't
# find <SDL2/SDL.h>.
EMFLAGS   := -std=c++17 -O3 -Wall -Wextra -DAI_PLAYER_HELPERS_ONLY -I.. \
    -s USE_SDL=2 -s USE_ZLIB=1
LDFLAGS   := \
    -s USE_SDL=2 \
    -s USE_WEBGL2=1 -s FULL_ES3=1 \
    -s MIN_WEBGL_VERSION=2 -s MAX_WEBGL_VERSION=2 \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s INITIAL_MEMORY=128MB \
    -s MINIFY_HTML=0 \
    -s USE_ZLIB=1 \
    -s EXPORTED_RUNTIME_METHODS=ccall,UTF8ToString \
    -s EXPORTED_FUNCTIONS=_chess_start,_chess_resize,_on_ai_move_from_js,_on_eval_from_js \
    --preload-file ../models-web-packed@/models \
    --preload-file ../challenges@/challenges \
    --preload-file ../sounds@/sounds \
    --preload-file DejaVuSans-Bold.ttf@/DejaVuSans-Bold.ttf \
    --shell-file index.html

# Sources compiled from the parent dir (shared with desktop build).
SHARED_SRCS := \
    ../chess_types.cpp \
    ../chess_rules.cpp \
    ../game_state.cpp \
    ../app_state.cpp \
    ../board_renderer.cpp \
    ../cloth_flag.cpp \
    ../linalg.cpp \
    ../stl_model.cpp \
    ../shader.cpp \
    ../challenge.cpp \
    ../ai_player.cpp \
    ../time_control.cpp \
    ../audio.cpp \
    ../compression.cpp

# Web-only sources.
WEB_SRCS := main_web.cpp ai_player_web.cpp font_atlas_stb.cpp

# Object files live in build/ so `make -jN` can compile each TU in
# parallel. em++ is clang under the hood — -j20 actually parallelizes
# once the build is split into per-TU compile steps instead of a single
# monolithic em++ invocation.
OBJ_DIR   := build
SHARED_OBJS := $(patsubst ../%.cpp,$(OBJ_DIR)/shared_%.o,$(SHARED_SRCS))
WEB_OBJS    := $(patsubst %.cpp,$(OBJ_DIR)/web_%.o,$(WEB_SRCS))
OBJS        := $(SHARED_OBJS) $(WEB_OBJS)

ARTIFACTS := chess.html chess.js chess.wasm chess.data

all: chess.html

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/shared_%.o: ../%.cpp | $(OBJ_DIR)
	$(EMCC) $(EMFLAGS) -c $< -o $@

$(OBJ_DIR)/web_%.o: %.cpp | $(OBJ_DIR)
	$(EMCC) $(EMFLAGS) -c $< -o $@

chess.html: $(OBJS) stockfish-bridge.js index.html stockfish/stockfish.js
	$(EMCC) $(EMFLAGS) $(OBJS) -o $@ $(LDFLAGS)
	@# Substitute the cache-bust hash placeholder in the generated
	@# chess.html. index.html uses __BUILD_HASH__ as a literal token
	@# that won't collide with anything Emscripten produces.
	sed -i.bak 's/__BUILD_HASH__/$(BUILD_HASH)/g' $@ && rm $@.bak

stockfish/stockfish.js:
	@echo "Vendored stockfish.js missing under web/stockfish/."
	@echo "These are committed to the repo; run \`git checkout\` to restore."
	@exit 1

clean:
	rm -f $(ARTIFACTS)
	rm -rf $(OBJ_DIR)

# Optional: serve the build locally for testing.
serve: all
	@echo "Serving on http://localhost:8000/chess.html"
	python3 -m http.server 8000

.PHONY: all clean serve
