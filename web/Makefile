EMCC      := em++
EMFLAGS   := -std=c++17 -O3 -Wall -Wextra -DAI_PLAYER_HELPERS_ONLY -I..
LDFLAGS   := \
    -s USE_SDL=2 \
    -s USE_WEBGL2=1 -s FULL_ES3=1 \
    -s MIN_WEBGL_VERSION=2 -s MAX_WEBGL_VERSION=2 \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s INITIAL_MEMORY=128MB \
    -s MINIFY_HTML=0 \
    -s EXPORTED_RUNTIME_METHODS=ccall,UTF8ToString \
    -s EXPORTED_FUNCTIONS=_on_ai_move_from_js,_on_eval_from_js \
    --preload-file ../models@/models \
    --preload-file ../challenges@/challenges \
    --preload-file DejaVuSans-Bold.ttf@/DejaVuSans-Bold.ttf \
    --shell-file index.html

# Sources compiled from the parent dir (shared with desktop build).
SHARED_SRCS := \
    ../chess_types.cpp \
    ../chess_rules.cpp \
    ../board_renderer.cpp \
    ../linalg.cpp \
    ../stl_model.cpp \
    ../shader.cpp \
    ../challenge.cpp \
    ../ai_player.cpp

# Web-only sources.
WEB_SRCS := main_web.cpp ai_player_web.cpp font_atlas_stb.cpp

ARTIFACTS := chess.html chess.js chess.wasm chess.data

all: chess.html

chess.html: $(SHARED_SRCS) $(WEB_SRCS) stockfish-bridge.js index.html stockfish/stockfish.js
	$(EMCC) $(EMFLAGS) $(SHARED_SRCS) $(WEB_SRCS) -o $@ $(LDFLAGS)

stockfish/stockfish.js:
	@echo "Vendored stockfish.js missing under web/stockfish/."
	@echo "These are committed to the repo; run \`git checkout\` to restore."
	@exit 1

clean:
	rm -f $(ARTIFACTS)

# Optional: serve the build locally for testing.
serve: all
	@echo "Serving on http://localhost:8000/chess.html"
	python3 -m http.server 8000

.PHONY: all clean serve
