# Claude instructions for this repo

## README sync

Whenever a change lands that could make `README.md` stale, update
`README.md` as part of the same change. In particular, re-check the
README before pushing if any of these happen:

- A file is **added, renamed, or removed** (the Project Structure
  tree lists every top-level source file).
- A **build step** changes (Makefile inputs, new `-s` emscripten
  flags, new tool invocation).
- A **user-visible feature** is added, removed, or reworked
  (anything that would change the Features / Controls / How-to-play
  sections).
- An **asset pipeline** step changes (tools under `tools/`,
  `models*/`, `sounds/`, preload list in `web/Makefile`).
- A **platform limitation** is lifted or newly introduced (the
  "Limitations vs the desktop build" list).

If the change is purely internal and no user-facing or layout
details move, leave the README alone and say so in the response.
