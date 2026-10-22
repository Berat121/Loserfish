#!/usr/bin/env python3
"""Loserfish speed benchmark. Standard library only (Windows, Linux, macOS).

Measures wall time to reach a fixed depth, so it captures both raw speed and Lazy SMP scaling.
Run it on a quiet machine (close other programs); the best of --repeats runs is used.

  python3 tests/bench.py src/loserfish                       # threads 1..cores, depth 10
  python3 tests/bench.py bin/lf-base bin/lf-bmi2 --threads 1  # compare builds (ARCH, PGO)
  python3 tests/bench.py src/loserfish --threads 1,2,4,8 --depth 11 --json out.json

Columns: nodes and nps are totals over all positions, time is the sum of per-position best times,
speedup is time(1 thread) / time(N threads) for the same binary. With more than one thread the
node count is not deterministic, so only time-to-depth is comparable; nps shows raw throughput.
"""
import argparse, json, os, queue, subprocess, sys, threading, time

FENS = [
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r1bq1rk1/pp2bppp/2n1pn2/3p4/2PP4/2N1PN2/PP2BPPP/R1BQ1RK1 w - - 0 9",
    "r2q1rk1/pp2ppbp/2np1np1/2p5/2P1P1b1/2N1BN2/PP2BPPP/R2QK2R w KQ - 0 9",
    "6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1",
]

class Engine:
    def __init__(self, path):
        self.p = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.q = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()
        self.send("uci"); self.wait("uciok", 20)

    def _read(self):
        for line in self.p.stdout:
            self.q.put(line.strip())

    def send(self, s):
        self.p.stdin.write(s + "\n"); self.p.stdin.flush()

    def wait(self, prefix, timeout):
        end = time.time() + timeout
        last = None
        while time.time() < end:
            try:
                line = self.q.get(timeout=0.2)
            except queue.Empty:
                continue
            if line.startswith("info depth"):
                last = line
            if line.startswith(prefix):
                return line, last
        raise TimeoutError(prefix)

    def close(self):
        try:
            self.send("quit"); self.p.wait(5)
        except Exception:
            self.p.kill()

def run_one(path, fen, depth, threads, hash_mb, timeout):
    e = Engine(path)
    try:
        e.send(f"setoption name Threads value {threads}")
        e.send(f"setoption name Hash value {hash_mb}")
        e.send("isready"); e.wait("readyok", 30)
        e.send(f"position fen {fen}")
        t0 = time.perf_counter()
        e.send(f"go depth {depth}")
        best, last = e.wait("bestmove", timeout)
        dt = time.perf_counter() - t0
        nodes = 0
        if last:
            t = last.split()
            if "nodes" in t:
                nodes = int(t[t.index("nodes") + 1])
        return dt, nodes, best.split()[1]
    finally:
        e.close()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("engines", nargs="+")
    ap.add_argument("--threads", default=None, help="comma list, default 1,2,4.. up to CPU count")
    ap.add_argument("--depth", type=int, default=10)
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--hash", type=int, default=256)
    ap.add_argument("--fens", help="file with one FEN per line (replaces the built-in set)")
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--json")
    a = ap.parse_args()

    cpus = os.cpu_count() or 1
    if a.threads:
        tlist = [int(x) for x in a.threads.split(",")]
    else:
        tlist, n = [], 1
        while n < cpus:
            tlist.append(n); n *= 2
        tlist.append(cpus)
    fens = [l.strip() for l in open(a.fens) if l.strip()] if a.fens else FENS

    print(f"cpus={cpus} depth={a.depth} positions={len(fens)} repeats={a.repeats} hash={a.hash}MB")
    print(f"{'engine':28s} {'thr':>3s} {'nodes':>12s} {'time_s':>8s} {'Mnps':>7s} {'speedup':>8s}  bestmoves-differ")
    results = []
    for path in a.engines:
        base_time = None
        ref_moves = None
        for thr in tlist:
            tot_t = tot_n = 0.0
            moves = []
            for fen in fens:
                runs = [run_one(path, fen, a.depth, thr, a.hash, a.timeout) for _ in range(a.repeats)]
                bt, bn, bm = min(runs, key=lambda r: r[0])
                tot_t += bt; tot_n += bn; moves.append(bm)
            if base_time is None:
                base_time, ref_moves = tot_t, moves
            differ = sum(1 for x, y in zip(moves, ref_moves) if x != y)
            sp = base_time / tot_t if tot_t else 0
            name = os.path.basename(path)[:28]
            print(f"{name:28s} {thr:3d} {int(tot_n):12d} {tot_t:8.2f} {tot_n / tot_t / 1e6:7.2f} {sp:8.2f}  {differ}/{len(fens)}")
            results.append(dict(engine=path, threads=thr, nodes=int(tot_n), time=tot_t,
                                nps=tot_n / tot_t, speedup=sp, bestmoves=moves))
    if a.json:
        with open(a.json, "w") as f:
            json.dump(dict(cpus=cpus, depth=a.depth, results=results), f, indent=1)

if __name__ == "__main__":
    main()
