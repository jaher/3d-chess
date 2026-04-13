# Vendored Stockfish.js

These prebuilt files come from the [nmrugg/stockfish.js](https://github.com/nmrugg/stockfish.js) v18.0.0 release:

- **`stockfish.js`** — JavaScript loader (~20 KB)
  - Source: <https://github.com/nmrugg/stockfish.js/releases/download/v18.0.0/stockfish-18-lite-single.js>
- **`stockfish.wasm`** — WebAssembly binary (~7.3 MB)
  - Source: <https://github.com/nmrugg/stockfish.js/releases/download/v18.0.0/stockfish-18-lite-single.wasm>

This is the **lite single-threaded** variant of Stockfish 18. It was chosen because:

1. Single-threaded → does not require `SharedArrayBuffer` / COOP-COEP HTTP headers, so it runs on plain GitHub Pages.
2. Lite NNUE (~7 MB) is small enough for fast page loads while still being orders of magnitude stronger than any human player.

## Updating

To bump to a newer release, replace both files with the matching pair from a newer [nmrugg/stockfish.js release](https://github.com/nmrugg/stockfish.js/releases) and update the version number above.

## License

Stockfish is GPLv3. See <https://github.com/official-stockfish/Stockfish/blob/master/Copying.txt>.
