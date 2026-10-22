# Loserfish tests

Self-test of the misere search against plain minimax (pruning/table disabled):

    cd src
    g++ -std=c++17 -O2 -DNDEBUG -DIS_64BIT -msse -DUSE_SSE2 -msse2 -DARCH=x86-64 -pthread \
        -o ../tests/lose_selftest ../tests/lose_selftest.cpp attacks.cpp benchmark.cpp bitboard.cpp \
        evaluate.cpp misc.cpp movegen.cpp movepick.cpp position.cpp search.cpp thread.cpp timeman.cpp \
        tt.cpp uci.cpp ucioption.cpp tune.cpp syzygy/tbprobe.cpp engine.cpp score.cpp memory.cpp \
        losesearch.cpp misere.cpp
    MIS_DBG=12 ../tests/lose_selftest 4 < fens.txt      # one FEN per line

MIS_DBG bits: 1 = no table cutoffs, 2 = no mate distance pruning, 8 = ignore repetitions
(repetition makes table hits path dependent, so exact equality is only expected with bit 8).

Play-out tests (needs python-chess):

    python3 loserfish_play.py 12 normal adversarial 0.1   # vs a normal engine
    python3 loserfish_play.py 8 adv adversarial 0.1       # both sides want to be mated

## uci_protocol_test.py

Needs python-chess. Checks: positions where every legal move mates (legal move returned, both
opponent modes, 1 and 2 threads), searchmoves, go infinite + stop, movetime, ponder + ponderhit/stop,
Chess960 legality. Usage: `python3 tests/uci_protocol_test.py src/loserfish`.

## bench.py and build_variants.sh

`python3 tests/bench.py bin/lf-a bin/lf-b --threads 1` compares binaries; without `--threads` it runs
1, 2, 4 .. up to the CPU count and prints the time-to-depth speedup. `tests/build_variants.sh [out_dir]`
builds ARCH variants (override with `ARCHS=...`) and a PGO build into `bin/` (ignored by git).

## Stalemate tests

`lose_stalemate_fens.txt` (stalemate positions) and `lose_stalemate_parent_fens.txt` (positions with a move
into stalemate). A stalemate at the search horizon must score 0 (a draw), not the static evaluation:

    EXPECT_ZERO=1 MIS_DBG=12 tests/lose_selftest 0 < tests/lose_stalemate_fens.txt
    MIS_DBG=12 tests/lose_selftest 1 < tests/lose_stalemate_parent_fens.txt   # also depths 2 and 3

Note: the selftest compares the optimized search with a plain minimax that shares the evaluation and the
leaf rules, so it cannot find a mistake in the rules themselves. `EXPECT_ZERO` checks the rule directly.

`lose_allmate_fens.txt`: positions where every legal move mates. With the recursion bound lowered to ply 1
the delivered mate must still score as a mate, not a draw:

    EXPECT_VALUE=-29999 MIS_PLYCAP=1 MIS_DBG=12 tests/lose_selftest 3 < tests/lose_allmate_fens.txt
