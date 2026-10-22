#!/bin/bash
# Checks node counts of "go perft" against known values. Usage: tests/perft.sh [path/to/loserfish]
ENGINE=${1:-$(dirname "$0")/../src/loserfish}
fail=0
check() { # fen depth expected
  got=$(printf 'position fen %s\ngo perft %s\n' "$1" "$2" | "$ENGINE" 2>&1 | grep -i "nodes searched" | grep -o '[0-9]*$')
  if [ "$got" = "$3" ]; then echo "ok   depth $2 $3"; else echo "FAIL depth $2 expected $3 got '$got' ($1)"; fail=1; fi
}
check "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1" 5 4865609
check "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1" 4 4085603
check "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1" 5 674624
check "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1" 4 422333
check "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8" 4 2103487
exit $fail
