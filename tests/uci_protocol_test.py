#!/usr/bin/env python3
"""UCI protocol and edge-case tests for Loserfish. Needs python-chess.
usage: uci_protocol_test.py [path/to/loserfish]   (exit code 1 on any failure)"""
import os, subprocess, sys, time, threading, queue
import chess

ENGINE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "loserfish")

class Eng:
    def __init__(self):
        self.p = subprocess.Popen([ENGINE], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.q = queue.Queue()
        threading.Thread(target=self._rd, daemon=True).start()
        self.send("uci"); self.wait("uciok", 10)
    def _rd(self):
        for line in self.p.stdout: self.q.put(line.strip())
    def send(self, s): self.p.stdin.write(s + "\n"); self.p.stdin.flush()
    def wait(self, prefix, timeout):
        end = time.time() + timeout
        while time.time() < end:
            try: line = self.q.get(timeout=0.1)
            except queue.Empty: continue
            if line.startswith(prefix): return line
        return None
    def close(self):
        try: self.send("quit"); self.p.wait(5)
        except Exception: self.p.kill()

fails = 0
def check(name, ok, detail=""):
    global fails
    print(("ok   " if ok else "FAIL ") + name + (" " + detail if detail and not ok else ""))
    if not ok: fails += 1

def best(line): return line.split()[1] if line else None

# 1. Positions where every legal move mates: the engine must still return a legal move.
TRAPS = ["K7/1q3Q2/k4P2/8/8/1N4P1/8/8 w - - 0 1", "K1k5/1q6/Q7/8/5Q2/3Q4/8/8 w - - 0 1", "8/1B6/8/8/R7/Q7/q7/k1r4K w - - 0 1"]
for opp in ("adversarial", "cooperative"):
    for thr in (1, 2):
        for fen in TRAPS:
            e = Eng(); e.send(f"setoption name LoseOpponent value {opp}"); e.send(f"setoption name Threads value {thr}")
            e.send("isready"); e.wait("readyok", 10)
            e.send(f"position fen {fen}"); e.send("go depth 6")
            r = e.wait("bestmove", 20); b = chess.Board(fen)
            mv = best(r)
            legal = mv is not None and chess.Move.from_uci(mv) in b.legal_moves
            check(f"trap {opp} threads={thr} {fen[:12]}", legal, str(r)); e.close()

# 2. searchmoves restricts the root.
e = Eng(); e.send("position startpos"); e.send("go depth 5 searchmoves g1f3 b1c3")
r = e.wait("bestmove", 20); check("searchmoves", best(r) in ("g1f3", "b1c3"), str(r)); e.close()

# 3. go infinite stays running until stop.
e = Eng(); e.send("position startpos"); e.send("go infinite"); time.sleep(0.7)
early = e.wait("bestmove", 0.3); e.send("stop"); r = e.wait("bestmove", 5)
check("infinite waits for stop", early is None and r is not None, str((early, r))); e.close()

# 4. movetime is honored.
e = Eng(); e.send("position startpos"); t = time.time(); e.send("go movetime 300"); r = e.wait("bestmove", 5)
dt = time.time() - t; check("movetime 300ms", r is not None and dt < 0.9, f"{dt:.2f}s"); e.close()

# 5. ponder: stays searching until ponderhit, then answers.
e = Eng(); e.send("setoption name Ponder value true"); e.send("position startpos moves e2e4 e7e5"); e.send("go ponder wtime 5000 btime 5000")
time.sleep(0.5); early = e.wait("bestmove", 0.3); e.send("ponderhit"); r = e.wait("bestmove", 10)
check("ponder + ponderhit", early is None and r is not None, str((early, r))); e.close()
e = Eng(); e.send("position startpos"); e.send("go ponder wtime 5000 btime 5000"); time.sleep(0.4); e.send("stop"); r = e.wait("bestmove", 5)
check("ponder + stop", r is not None, str(r)); e.close()

# 5b. Cooperative mode: after ponderhit the clock starts counting (the search used to ignore it).
KIWI = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"
e = Eng(); e.send("setoption name LoseOpponent value cooperative"); e.send("setoption name Ponder value true")
e.send(f"position fen {KIWI}"); e.send("go ponder wtime 1000 btime 1000"); time.sleep(0.5)
early = e.wait("bestmove", 0.2); e.send("ponderhit"); t = time.time(); r = e.wait("bestmove", 15); dt = time.time() - t
check("cooperative ponderhit honors the clock", early is None and r is not None and dt < 1.0, f"{dt:.2f}s {r}"); e.close()

# 6. Chess960: castling-rights start position, moves must be legal in a 960 board.
for fen in ["bnrkqnbr/pppppppp/8/8/8/8/PPPPPPPP/BNRKQNBR w HChc - 0 1", "qrnbbnkr/pppppppp/8/8/8/8/PPPPPPPP/QRNBBNKR w HBhb - 0 1"]:
    e = Eng(); e.send("setoption name UCI_Chess960 value true"); e.send(f"position fen {fen}"); e.send("go depth 5")
    r = e.wait("bestmove", 20); b = chess.Board(fen, chess960=True); mv = best(r)
    ok = mv is not None and chess.Move.from_uci(mv) in b.legal_moves
    check(f"chess960 {fen[:8]}", ok, str(r))
    if ok:  # play a few plies to reach castling situations
        for _ in range(8):
            b.push(chess.Move.from_uci(mv)); 
            if b.is_game_over(): break
            e.send("position fen " + chess.Board(fen, chess960=True).fen() + " moves " + " ".join(m.uci() for m in b.move_stack)); e.send("go depth 4")
            r = e.wait("bestmove", 20); mv = best(r)
            if mv is None or chess.Move.from_uci(mv) not in b.legal_moves:
                check("chess960 line legal", False, f"{mv} in {b.fen()}"); break
        else: check("chess960 line legal", True)
    e.close()

# Time management: Move Overhead is taken off movetime and the clock, movestogo shortens the budget.
def timed(opp, overhead, go):
    e = Eng()
    e.send(f"setoption name LoseOpponent value {opp}")
    e.send(f"setoption name Move Overhead value {overhead}")
    e.send("isready"); e.wait("readyok", 10)
    e.send("position startpos moves e2e4 e7e5")
    t = time.time(); e.send(go)
    r = e.wait("bestmove", 15); dt = (time.time() - t) * 1000
    e.close()
    return r is not None, dt
for opp in ("adversarial", "cooperative"):
    ok, dt = timed(opp, 400, "go movetime 1000")
    check(f"{opp} movetime honors Move Overhead", ok and dt <= 750, f"took {dt:.0f} ms")
    ok, dt = timed(opp, 30, "go wtime 3000 btime 3000 movestogo 1")
    check(f"{opp} movestogo 1 stays inside the clock", ok and dt <= 2970, f"took {dt:.0f} ms")
    ok, dt = timed(opp, 2900, "go wtime 3000 btime 3000")
    check(f"{opp} huge overhead leaves time", ok and dt <= 200, f"took {dt:.0f} ms")

# go nodes bounds the total over all threads (helper threads used to run past the limit)
for thr in (1, 4):
    e = Eng(); e.send(f"setoption name Threads value {thr}")
    e.send("position fen r5nr/p1p5/1p1k2B1/1Q1p1K2/4P3/3P2q1/PP1P4/RNB5 b - - 0 25"); e.send("go nodes 200000")
    infos, bm, end = [], None, time.time() + 30
    while time.time() < end:
        try: line = e.q.get(timeout=0.2)
        except queue.Empty: continue
        if line.startswith("info depth"): infos.append(int(line.split(" nodes ")[1].split()[0]))
        if line.startswith("bestmove"): bm = line; break
    check(f"go nodes 200000 bounds total nodes threads={thr}", bm is not None and bool(infos) and max(infos) <= 220000, f"max {max(infos) if infos else None}")
    e.close()

print("FAILED" if fails else "ALL OK"); sys.exit(1 if fails else 0)
