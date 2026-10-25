# Loserfish

A UCI chess engine derived from [Stockfish](https://github.com/official-stockfish/Stockfish) whose goal
is the opposite of normal play: **to get checkmated**. It never wants to win; if every legal move mates,
it plays one only because a move is required.

## Model

Outcomes for the engine, best to worst: it is checkmated; draw; it checkmates the opponent.
By default the opponent is assumed to play the worst moves against that goal (it avoids mating us and
tries to force us to mate it). That is misere chess: zero-sum, so alpha-beta applies.
Option `LoseOpponent=cooperative` instead assumes the opponent mates whenever it can.

Search (`src/misere.cpp`): iterative deepening PVS, lock-free shared transposition table, mate distance
pruning with misere bounds, LMR, history/killers, free check evasions, Lazy SMP (`Threads`).
Details and status: `LOSERFISH.md`.

## Build

```
cd src
make -j build ARCH=x86-64 COMP=gcc      # produces src/loserfish (C++17)
# ARCH depends on your CPU: x86-64, x86-64-avx2, armv8, apple-silicon, ... ("make help" lists them)
```
CMake (no `sh` needed, GCC/Clang only):
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```
Verified on Linux (perft passes). On Windows (MinGW) it is unverified: use `-G "MinGW Makefiles"`.
MSVC is not supported. NNUE and nets were removed; no downloads needed.

## UCI options added

| Option | Default | Meaning |
|---|---|---|
| LoseMode | true | false falls back to the plain Stockfish search (material eval only) |
| LoseOpponent | adversarial | `adversarial` or `cooperative` |
| LoseScore | neutral | score shown to GUIs: `neutral` (always cp 0), `natural`, `inverted`. Neutral avoids GUI resign adjudication |
| LoseExtCap | 3 | adversarial search only: free check evasions allowed per path (0-99). Mutual checks otherwise never consume depth; 99 disables the cap |
| LoseSafetyDepth / LoseMateDepth | 6 / 8 | cooperative mode only |
| Threads, Hash | 1, 16 | Lazy SMP threads and table size |
| Move Overhead | 30 | ms taken off movetime and off the clock for GUI latency; `movestogo` is honored (budget = time / movestogo) |

## Speed

`make profile-build ARCH=<your arch> COMP=gcc` (PGO, uses `bench`, default depth 8) gave about 23% more nps
than the plain build in one x86-64 comparison (3.13 to 3.85 Mnps, same node counts).
`tests/build_variants.sh` builds ARCH variants plus PGO; `tests/bench.py` compares binaries and measures
Lazy SMP scaling (time to fixed depth). SMP scaling has not been measured on a multi-core machine yet.

## Tests

`tests/perft.sh` (node counts), `tests/lose_selftest.cpp` (optimized search equals plain minimax),
`tests/loserfish_play.py` (play-outs, needs python-chess). See `tests/README.md`. CI: `.github/workflows/ci.yml`.

## Limits (honest)

It cannot force a loss against an opponent who refuses to mate, except through zugzwang-like lines;
two adversarial Loserfish engines draw by repetition. Evaluation is simple and untuned. Ponder, infinite,
searchmoves, Chess960 and 32-bit builds are untested. Multi-thread speedup has not been measured yet; thread safety was checked with ThreadSanitizer.

## WebAssembly

Live demo: https://berat121.github.io/Loserfish/ (source in `docs/index.html`, `docs/app.js`; vendored chess.js and
coi-serviceworker under `docs/`).

Prebuilt browser files (about 520 KB `.wasm`, Emscripten 4.0.9): [`docs/loserfish.js`](docs/loserfish.js) and
[`docs/loserfish.wasm`](docs/loserfish.wasm). Load the script, create the engine with
`const M = await Loserfish({print: line => ...})`, then send UCI lines with
`M.ccall('loserfish_cmd', null, ['string'], ['go depth 8'])`. Engine output arrives in the `print` callback.

- The engine runs on a pthread, so the page needs cross-origin isolation (COOP/COEP headers) for SharedArrayBuffer.
- Rebuild with `scripts/build_wasm.sh web|node [outdir]` (Emscripten must be on PATH).
- `node tests/wasm_smoke.cjs build-wasm/loserfish.js` checks the Node build: depth 8 gives the same 810072 nodes as
  the native binary. The browser build is not tested in a browser yet.

## License

GNU GPL v3 (`Copying.txt`). Derived from Stockfish; see `AUTHORS` for the Stockfish developers.
