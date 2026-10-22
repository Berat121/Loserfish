# LOSERFISH.md

Design and status document for Loserfish: a UCI chess engine derived from Stockfish whose goal is to
lose, i.e. to get checkmated, and never to win. This file holds the design and status.

## 1. Rules of the game the engine plays

Outcomes from the engine's point of view, best to worst:

1. The engine is checkmated (the goal).
2. Draw (stalemate, threefold repetition, 50-move rule, insufficient material).
3. The engine checkmates the opponent (must never happen unless it is the only legal move).

The engine must never be the side that wins. When every legal move mates, it plays one, since a legal
move is required.

## 2. Opponent model

Design decision: the opponent plays the worst moves against the engine's goal. "Worst" means the opponent
avoids mating us, and tries to force us to mate it. That makes the game exactly misere chess:

- The side that gets checkmated wins; the side that delivers mate loses.
- The game is zero-sum, so minimax and alpha-beta are valid.
- Both sides use the same evaluation function, seen from the side to move.

Two modes, option LoseOpponent:

- adversarial (default): the model above. Robust against any opponent, but cannot force a loss against
  an opponent who refuses to mate, unless a forcing line (zugzwang, forced capture chains) exists.
- cooperative: the opponent is assumed to mate us whenever it can. Fast against engines that try to win,
  useless against opponents who decline mate. Kept for comparison and as a fallback.

## 3. Source layout

Stockfish file names are unchanged. NNUE, incbin, the universal build and all Dirty* bookkeeping were
removed from the upstream tree. Added or changed files:

- src/misere.cpp, src/misere.h: the adversarial search (default).
- src/losesearch.cpp, src/losesearch.h: dispatcher (choose) and the cooperative mode (mate search plus
  safety search).
- src/search.cpp: start_searching calls Lose::choose when LoseMode is on.
- src/engine.cpp: options LoseMode, LoseOpponent, LoseSafetyDepth, LoseMateDepth; ucinewgame clears state.
- src/evaluate.cpp: material-only evaluation for the leftover normal search (LoseMode=false).
- src/misc.cpp: UCI id name is Loserfish.
- src/Makefile: NNUE, net and universal targets removed; misere.cpp and losesearch.cpp added.
- tests/: lose_selftest.cpp, FEN lists, loserfish_play.py, uci_protocol_test.py, perft.sh, bench.py,
  build_variants.sh, README.md. CMakeLists.txt builds without a POSIX shell.

## 4. Adversarial search (misere.cpp)

Scores are from the side to move. Value MateScore - ply means the side to move is checkmated at ply
(a win). A move that delivers mate returns the negated value to the parent, so it is the worst result.
Stalemate and rule draws are 0.

Techniques:

- Iterative deepening with principal variation search, full window at the root, null windows elsewhere.
- Transposition table: 16-byte entries, 2-way buckets, full 64-bit key check, generation aging, mate
  scores stored relative to the node.
- Mate distance pruning with misere bounds. The best possible result at a node is being mated on the
  reply (ply + 2), or right now when in check (ply). The worst is delivering mate on the next ply.
- Check evasions are free in depth (check extension), bounded by the ply cap of 64 and repetition.
- Late move reductions for quiet, non-checking moves after the third, re-searched on a fail high.
- Move ordering: hash move, killers, butterfly history, small bonus for captures.
- Leaf rule: at depth 0 and not in check, return 0 if the side to move has no legal move (stalemate), else the
  static evaluation. A free, unattacked king step proves a legal move exists, so move generation is rarely needed.
- Repetition: is_draw(ply) inside the search, so table hits can be path dependent (known GHI effect).
- Early stop when a forced result is proven; time is soft-limited at the iteration boundary for time
  controls and hard-limited by the deadline.
- Lazy SMP: the Threads option sets the number of search threads. Helpers search with their own position
  and history and share the lock-free transposition table. Hash size follows the Hash option. History is
  halved each search and cleared on ucinewgame.

## 5. Evaluation

From the side to move: material deficit (opponent material minus own material), plus ten units per
weighted enemy piece standing in the 5x5 zone around the own king minus the same for the opponent
(pawn 1, minor 2, rook 3, queen 5), plus a small bonus when in check, plus a small term for each missing flight square of the own king
(fewer flight squares means closer to being mated). Higher means closer to being
mated. It is cheap on purpose (several million nodes per second per thread).

## 6. Cooperative mode (losesearch.cpp)

- Safety search: AND/OR search that rejects root moves after which the opponent can force a win or draw
  for us (plies set by LoseSafetyDepth, default 6).
- Mate search: iterative deepening helpmate-style search for the shortest line ending in our mate
  (LoseMateDepth, default 8), then a heuristic head-towards-loss search when no mate is found.

## 7. UCI behavior

- id name Loserfish. Scores: cp is the negated misere score, so being close to winning the game
  (mate) shows as negative cp, and "score mate -N" means we get mated in N moves. That is the
  LoseScore=natural view. With LoseScore=neutral (default) every score is shown as cp 0, so a GUI with resign
  adjudication never sees a lost position.
- go supports movetime, wtime/btime/winc/binc, depth, nodes, infinite, ponder, searchmoves.
  Ponder, infinite and searchmoves are covered by tests/uci_protocol_test.py.
- LoseMode=false falls back to the normal Stockfish search with the material-only evaluation.

## 8. UCI options added

- LoseMode (check, default true)
- LoseOpponent (string, adversarial or cooperative, default adversarial)
- LoseSafetyDepth (spin 1 to 12, default 6, cooperative mode)
- LoseMateDepth (spin 2 to 16, default 8, cooperative mode)
- LoseScore (string: neutral, natural or inverted, default neutral)
- Threads (existing) sets the number of search threads of the adversarial search.
- Hash (existing) sizes the table of the adversarial search.

## 9. Build

Linux and macOS: in src, run make -j build ARCH=x86-64 COMP=gcc (g++ 13 tested, C++17).

CMake (no POSIX shell needed, GCC or Clang): cmake -S . -B build -DCMAKE_BUILD_TYPE=Release, then
cmake --build build. Verified on Linux only. Windows (MinGW) and 32-bit builds are unverified.

## 10. Tests and results

- Perft: start position depth 5, Kiwipete depth 4, two more standard positions all match the known
  counts after the Dirty* removal. Bench node count before and after the removal was identical
  (2,567,548).
- Selftest (tests/lose_selftest.cpp): the optimized search in exact mode equals plain minimax in 363
  positions at depth 3 and 4 and in 40 positions at depth 5, zero mismatches, with repetitions ignored
  (MIS_DBG=12). With repetitions the only mismatches seen came from table hits across different paths.
- Play-outs (tests/loserfish_play.py, python-chess): 12 of 12 games lost against a normal engine at
  100 ms per move; 8 of 8 games between two adversarial Loserfish engines ended in threefold repetition
  with nobody winning; no illegal moves in any game.
- Games against a human who was also trying to lose: no checkmate happened. In one game the GUI marked
  "Black resigns" because of the score sign convention (the reason for the LoseScore option). The engine
  kept offering mate, then used forcing checks and sacrifices.

## 11. Known limits

- Cannot force a loss against an opponent who refuses to mate, apart from zugzwang-like forced lines;
  two adversarial engines draw by repetition.
- Syzygy code is still compiled in but unused in lose mode.
- Evaluation is simple; only one tuning experiment (flight squares) has been run, on a small sample.
- Multi-thread speedup has not been measured on a multi-core machine; correctness was checked with
  ThreadSanitizer.
- Windows and 32-bit builds are unverified.

## 12. Roadmap

1. Measure Lazy SMP scaling and ARCH/PGO builds (tests/bench.py, tests/build_variants.sh); consider
   best-thread selection.
2. Evaluation: opponent mobility, king exposure, tuning by self-play with larger samples.
3. Search for forcing lines against refusing opponents (needs more depth, so speed first).

## 13. License and credits

Loserfish is derived from Stockfish and is distributed under the GNU General Public License v3
(Copying.txt). The Stockfish developers are credited in AUTHORS. Keep all upstream copyright notices.

## 14. Change log

- Lazy SMP: Threads option; helpers have own Position copies and history; shared table of two relaxed
  atomic words per slot (key xor data), so there are no data races (ThreadSanitizer: 0 warnings).
- LoseScore option: neutral (default, always cp 0), natural, inverted.
- Evaluation: added own-king flight-square term.
- Staged move generation was tried and rejected (more nodes, slower); flag kStaged in misere.cpp.
- Executable renamed to loserfish; version 1.0; upstream files and workflows removed; CI added.
- Tried after that: TT prefetch and counter-move ordering (kept); insertion sort, eval cache, null move
  (rejected). Flags kPrefetch, kCounter, kInsertion, kEvalCache, kNull in misere.cpp.
- Bench: default depth lowered to 8 (the old depth 13 takes hours with this search) and its node count now
  includes the Loserfish search. A PGO build measured +23% nps in one comparison.
- Stalemate at the horizon is now scored as a draw (it used to return the static evaluation, in both the search
  and the reference minimax, so the selftest could not see it). Tests: tests/lose_stalemate*_fens.txt.
- Cooperative mode now honors ponderhit like the adversarial search. Note on its safety scan: "safe" means safe
  within LoseSafetyDepth plies and MaxLosePly, not a proof that the loss is forced at any depth.
- The recursion bound (ply 64) now classifies a checkmate (we are mated: the goal) and a stalemate before
  falling back to the evaluation; it used to score a mate there as a draw. Test-only MIS_PLYCAP lowers the bound,
  tests/lose_allmate_fens.txt checks the value with EXPECT_VALUE=-29999 MIS_PLYCAP=1.
