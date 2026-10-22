/*
  Loserfish, a UCI chess engine derived from Stockfish
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)

  Loserfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Loserfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "misere.h"
#include "misc.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "attacks.h"
#include "bitboard.h"
#include "movegen.h"
#include "uci.h"

namespace Stockfish::Misere {

namespace {

constexpr int MaxPly    = 64;  // hard bound for recursion and all per-ply arrays
constexpr int Inf       = 32000;
constexpr int MateScore = 30000;
constexpr int MateInMax = MateScore - MaxPly;
constexpr bool kPrefetch  = true;   // experiment flags, measured one by one
constexpr bool kCounter   = true;
constexpr bool kInsertion = false;
constexpr bool kEvalCache = false;
constexpr bool kNull      = false;  // unsound in misere (zugzwang is our weapon); tested only
constexpr bool kStaged  = false;  // false: generate all moves at once (for measurement)

// ---------------------------------------------------------------------------
// Transposition table: 16-byte entries, 2-way buckets, full 64-bit key check.
// ---------------------------------------------------------------------------

enum Bound : std::uint8_t {
    BOUND_NONE  = 0,
    BOUND_UPPER = 1,
    BOUND_LOWER = 2,
    BOUND_EXACT = 3
};

// Decoded table data.
struct TTData {
    int   score    = 0;
    Move  move     = Move::none();
    int   depth    = 0;
    int   genBound = 0;  // generation (6 bits) | bound (2 bits)

    Bound bound() const { return Bound(genBound & 3); }
    int   gen() const { return genBound >> 2; }
};

// A slot is two relaxed atomic words. The first stores key ^ data, so a reader that
// sees a torn pair (two threads writing the same slot) fails the key check and treats
// it as a miss. No plain shared writes exist, so helper threads are free of data races.
struct Slot {
    std::atomic<u64> keyx{0};
    std::atomic<u64> data{0};
};
static_assert(sizeof(Slot) == 16);

inline u64 pack(const TTData& d) {
    return u64(std::uint16_t(std::int16_t(d.score))) | (u64(d.move.raw()) << 16)
         | (u64(std::uint8_t(std::int8_t(d.depth))) << 32) | (u64(std::uint8_t(d.genBound)) << 40);
}

inline TTData unpack(u64 w) {
    TTData d;
    d.score    = std::int16_t(std::uint16_t(w & 0xFFFF));
    d.move     = Move(std::uint16_t((w >> 16) & 0xFFFF));
    d.depth    = std::int8_t(std::uint8_t((w >> 32) & 0xFF));
    d.genBound = int((w >> 40) & 0xFF);
    return d;
}

class Table {
   public:
    // Not thread safe: only called while no search thread runs.
    void resize(std::size_t mb) {
        std::size_t n = 2;
        while ((n * 2) * sizeof(Slot) <= (mb << 20))
            n *= 2;
        if (n != count)
        {
            slots.reset(new Slot[n]);
            count = n;
        }
        mask = n - 1;
    }

    void clear() {
        for (std::size_t i = 0; i < count; ++i)
        {
            slots[i].keyx.store(0, std::memory_order_relaxed);
            slots[i].data.store(0, std::memory_order_relaxed);
        }
        generation = 0;
    }

    void prefetch(u64 key) const { Stockfish::prefetch(&slots[key & mask]); }

    void new_search() { generation = (generation + 1) & 63; }

    bool probe(u64 key, TTData& out) const {
        for (std::size_t i = 0; i < 2; ++i)
        {
            const Slot& s = slots[(key & mask) ^ i];
            const u64   d = s.data.load(std::memory_order_relaxed);
            const u64   k = s.keyx.load(std::memory_order_relaxed);
            if (d != 0 && (k ^ d) == key)
            {
                out = unpack(d);
                return true;
            }
        }
        return false;
    }

    void store(u64 key, int score, Bound b, int depth, Move m) {
        Slot* s0 = &slots[key & mask];
        Slot* s1 = &slots[(key & mask) ^ 1];
        const u64 d0 = s0->data.load(std::memory_order_relaxed);
        const u64 k0 = s0->keyx.load(std::memory_order_relaxed);
        const u64 d1 = s1->data.load(std::memory_order_relaxed);
        const u64 k1 = s1->keyx.load(std::memory_order_relaxed);

        Slot* t   = nullptr;
        u64   old = 0;
        if (d0 != 0 && (k0 ^ d0) == key)
            t = s0, old = d0;
        else if (d1 != 0 && (k1 ^ d1) == key)
            t = s1, old = d1;
        else
        {
            auto worth = [&](u64 w) {
                const TTData e = unpack(w);
                return e.depth - 8 * ((generation - e.gen()) & 63);
            };
            t = worth(d0) <= worth(d1) ? s0 : s1;
        }

        if (old)
        {
            const TTData e = unpack(old);
            if (m == Move::none())
                m = e.move;
            if (b != BOUND_EXACT && depth < e.depth - 3 && e.gen() == generation)
                return;
        }

        TTData n;
        n.score    = score;
        n.move     = m;
        n.depth    = depth;
        n.genBound = (generation << 2) | int(b);
        const u64 w = pack(n);
        t->data.store(w, std::memory_order_relaxed);
        t->keyx.store(key ^ w, std::memory_order_relaxed);
    }

   private:
    std::unique_ptr<Slot[]> slots = std::unique_ptr<Slot[]>(new Slot[2]);
    std::size_t             count = 2;
    std::size_t             mask  = 1;
    int                     generation = 0;
};

// History is per search thread; it persists between searches (halved at each start).
struct History {
    int h[COLOR_NB][SQUARE_NB][SQUARE_NB];
};

Table                                 gTable;
std::vector<std::unique_ptr<History>> gHists;

History& hist_for(std::size_t i) {
    while (gHists.size() <= i)
        gHists.push_back(std::make_unique<History>());  // value-initialized: all zero
    return *gHists[i];
}

inline int to_tt(int v, int ply) { return v >= MateInMax ? v + ply : v <= -MateInMax ? v - ply : v; }
inline int from_tt(int v, int ply) { return v >= MateInMax ? v - ply : v <= -MateInMax ? v + ply : v; }

// ---------------------------------------------------------------------------
// Evaluation: how close the side to move is to being mated, minus how close the
// opponent is. Material deficit and enemy pieces near our own king are good.
// ---------------------------------------------------------------------------

Bitboard KingZone[SQUARE_NB];
bool     tablesReady = false;

void init_tables() {
    if (tablesReady)
        return;
    for (int s = 0; s < SQUARE_NB; ++s)
    {
        Bitboard z = 0;
        for (int df = -2; df <= 2; ++df)
            for (int dr = -2; dr <= 2; ++dr)
            {
                const int f = (s & 7) + df, r = (s >> 3) + dr;
                if (f >= 0 && f < 8 && r >= 0 && r < 8)
                    z |= square_bb(make_square(File(f), Rank(r)));
            }
        KingZone[s] = z;
    }
    tablesReady = true;
}

inline bool insufficient_material(const Position& pos) {
    return !pos.count<PAWN>() && !pos.count<ROOK>() && !pos.count<QUEEN>()
        && pos.count<KNIGHT>() + pos.count<BISHOP>() <= 1;
}

inline int pressure(const Position& pos, Color c) {
    const Bitboard z = KingZone[pos.square<KING>(c)];
    const Color    o = ~c;
    return popcount(z & pos.pieces(o, PAWN)) + 2 * popcount(z & pos.pieces(o, KNIGHT, BISHOP))
         + 3 * popcount(z & pos.pieces(o, ROOK)) + 5 * popcount(z & pos.pieces(o, QUEEN));
}

inline int evaluate(const Position& pos) {
    const Color us = pos.side_to_move(), them = ~us;
    const int   matUs   = PawnValue * pos.count<PAWN>(us) + int(pos.non_pawn_material(us));
    const int   matThem = PawnValue * pos.count<PAWN>(them) + int(pos.non_pawn_material(them));
    int         e       = matThem - matUs + 10 * (pressure(pos, us) - pressure(pos, them));
    if (pos.checkers())
        e += 25;

    // Fewer flight squares for our king means closer to being mated. threats_by() holds
    // the squares attacked by the opponent of the side to move, i.e. attacked from our view.
    const Bitboard flightSq = Attacks::attacks_bb(KING, pos.square<KING>(us)) & ~pos.pieces(us)
                            & ~pos.threats_by(ALL_PIECES);
    e += 7 * (4 - std::min(popcount(flightSq), 4));
    return e;
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

struct RootEntry {
    Move m;
    int  score;
};

class Searcher {
   public:
    Searcher(Position& p, const std::atomic_bool& s, Table& t, History& h, bool exactMode) :
        pos(p),
        stop(s),
        tt(t),
        hist(h),
        exact(exactMode) {
        std::memset(killers, 0, sizeof(killers));
        std::memset(counter, 0, sizeof(counter));
        std::memset(curMove, 0, sizeof(curMove));
        if (kEvalCache)
            ecache.reset(new EvalEntry[EvalCacheSize]());
    }

    int search(int ply, int depth, int alpha, int beta, bool pvNode);
    int ref(int ply, int depth);
    int root(std::vector<RootEntry>& rms, int depth, std::vector<Move>& pvOut, bool& improved);

    Position&               pos;
    const std::atomic_bool& stop;
    Table&                  tt;
    History&                hist;
    bool                    exact;
    int                     plyCap = MaxPly;  // test only (MIS_PLYCAP): lowers the recursion bound
    int                     dbg = 0;  // test only: 1 = no TT cutoffs, 2 = no mate distance pruning, 8 = ignore repetitions

    StateInfo states[MaxPly + 2];
    Move      pv[MaxPly + 2][MaxPly + 2];
    int       pvLen[MaxPly + 2] = {};
    Move      killers[MaxPly + 2][2];
    Move      counter[SQUARE_NB][SQUARE_NB];  // reply that refuted the previous move (from, to)
    Move      curMove[MaxPly + 2];

    struct EvalEntry {
        u64 key;
        int e;
    };
    static constexpr std::size_t EvalCacheSize = 1 << 14;
    std::unique_ptr<EvalEntry[]> ecache;

    // Whether the side to move (not in check) has a legal move. A king step to a square that is
    // free and not attacked proves it without generating moves; only otherwise generate.
    bool has_legal_move() const {
        const Color    us = pos.side_to_move();
        const Bitboard flight = Attacks::attacks_bb(KING, pos.square<KING>(us)) & ~pos.pieces(us)
                              & ~pos.threats_by(ALL_PIECES);
        return flight || MoveList<ALL>(pos).size() != 0;
    }

    int eval() {
        if (!kEvalCache)
            return evaluate(pos);
        EvalEntry& c = ecache[pos.key() & (EvalCacheSize - 1)];
        if (c.key == pos.key())
            return c.e;
        c.key = pos.key();
        c.e   = evaluate(pos);
        return c.e;
    }

    u64       nodes     = 0;
    std::atomic<u64> pubNodes{0};  // nodes, published every 1024 nodes for other threads
    u64       nodeLimit = 0;
    TimePoint deadline  = 0;
    bool      aborted   = false;
    int       maxPly    = 0;

    // Main thread only: after "ponderhit" the time limits start counting.
    const std::atomic_bool* ponderFlag   = nullptr;
    TimePoint               ponderHardMs = 0;
    TimePoint               ponderHitAt  = 0;

    void check_ponderhit() {
        if (ponderFlag && !ponderFlag->load(std::memory_order_relaxed))
        {
            ponderFlag  = nullptr;
            ponderHitAt = now();
            if (ponderHardMs)
                deadline = ponderHitAt + ponderHardMs;
        }
    }

   private:
    // Test mode 8 ignores repetitions, which make table hits path dependent.
    bool drawn(int ply) const { return (dbg & 8) ? pos.rule50_count() > 99 : pos.is_draw(ply); }

    void poll() {
        if ((nodes & 1023) == 0)
        {
            pubNodes.store(nodes, std::memory_order_relaxed);
            check_ponderhit();
            if (stop.load(std::memory_order_relaxed))
                aborted = true;
            else if (deadline && now() >= deadline)
                aborted = true;
        }
        if (nodeLimit && nodes >= nodeLimit)
            aborted = true;
    }
};

// Plain minimax with the same node semantics (draws, leaf rule, free check evasions,
// ply cap), no pruning and no table. Used only to validate the optimized search.
int Searcher::ref(int ply, int depth) {
    const bool inCheck = bool(pos.checkers());
    if (ply > 0)
    {
        if (insufficient_material(pos) || drawn(ply))
            return 0;
        if (ply >= plyCap)
        {
            // At the recursion bound we cannot search on, but a mate or a stalemate is still terminal.
            if (MoveList<ALL>(pos).size() == 0)
                return inCheck ? MateScore - ply : 0;
            return inCheck ? 0 : evaluate(pos);
        }
    }
    if (depth <= 0 && !inCheck)
        return MoveList<ALL>(pos).size() == 0 ? 0 : evaluate(pos);  // no moves, no check: stalemate

    const MoveList<ALL> ml(pos);
    if (ml.size() == 0)
        return inCheck ? MateScore - ply : 0;

    int best = -Inf;
    for (const auto& e : ml)
    {
        const Move m = e;
        pos.do_move(m, states[ply], pos.gives_check(m), nullptr, nullptr);
        const int v = -ref(ply + 1, depth - 1 + (inCheck ? 1 : 0));
        pos.undo_move(m);
        best = std::max(best, v);
    }
    return best;
}

int Searcher::search(int ply, int depth, int alpha, int beta, bool pvNode) {

    pvLen[ply] = 0;
    ++nodes;
    poll();
    if (aborted)
        return 0;
    if (kPrefetch)
        tt.prefetch(pos.key());
    if (ply > maxPly)
        maxPly = ply;

    const bool inCheck = bool(pos.checkers());

    if (ply > 0)
    {
        if (insufficient_material(pos) || drawn(ply))
            return 0;
        if (ply >= plyCap)
        {
            if (MoveList<ALL>(pos).size() == 0)
                return inCheck ? MateScore - ply : 0;
            return inCheck ? 0 : eval();
        }

        // Mate distance pruning. Worst case: we deliver mate on the next ply.
        // Best case: we are mated (now if in check, otherwise on the reply to our move).
        if (!(dbg & 2))
        {
            alpha = std::max(alpha, -MateScore + ply + 1);
            beta  = std::min(beta, inCheck ? MateScore - ply : MateScore - ply - 2);
            if (alpha >= beta)
                return alpha;
        }
    }

    if (depth <= 0 && !inCheck)
        return has_legal_move() ? eval() : 0;  // stalemate is a draw, not an evaluation

    const u64 key    = pos.key();
    TTData    tte;
    Move      ttMove = Move::none();
    if (tt.probe(key, tte))
    {
        ttMove = tte.move;
        if (!(dbg & 1) && ply > 0 && !pvNode && (exact ? tte.depth == depth : tte.depth >= depth))
        {
            const int   s = from_tt(tte.score, ply);
            const Bound b = tte.bound();
            if (b == BOUND_EXACT || (b == BOUND_LOWER && s >= beta) || (b == BOUND_UPPER && s <= alpha))
            {
                return s;
            }
        }
    }

    if (kNull && !exact && !pvNode && !inCheck && ply > 0 && depth >= 3
        && curMove[ply - 1] != Move::null() && pos.non_pawn_material(pos.side_to_move()) > 0
        && eval() >= beta)
    {
        curMove[ply] = Move::null();
        pos.do_null_move(states[ply]);
        const int v = -search(ply + 1, depth - 1 - (3 + depth / 4), -beta, -beta + 1, false);
        pos.undo_null_move();
        if (aborted)
            return 0;
        if (v >= beta && v < MateInMax && v > -MateInMax)
            return v;
    }

    const Color us   = pos.side_to_move();
    const Move  prev = ply > 0 ? curMove[ply - 1] : Move::none();
    Move        moves[MAX_MOVES];
    int         scores[MAX_MOVES];
    int         n = 0;

    auto add = [&](const Move* b, const Move* e) {
        for (const Move* p = b; p != e; ++p)
        {
            const Move m = *p;
            int        sc;
            if (m == ttMove)
                sc = 1 << 30;
            else if (m == killers[ply][0])
                sc = 1 << 20;
            else if (m == killers[ply][1])
                sc = (1 << 20) - 1;
            else if (kCounter && m == counter[prev.from_sq()][prev.to_sq()])
                sc = (1 << 20) - 2;
            else
                sc = hist.h[us][m.from_sq()][m.to_sq()] + (pos.capture(m) ? 2000 : 0);
            moves[n]    = m;
            scores[n++] = sc;
        }
    };

    // Staged generation: captures first; quiets only when no capture cuts off.
    // Check evasions are generated in one go. Every list holds legal moves only.
    int stage;
    if (inCheck)
    {
        const MoveList<EVASIONS> ml(pos);
        add(ml.begin(), ml.end());
        stage = 2;
    }
    else
    {
        const MoveList<CAPTURES> ml(pos);
        add(ml.begin(), ml.end());
        stage = kStaged ? 1 : 2;
        if (!kStaged)
        {
            const MoveList<QUIETS> ql(pos);
            add(ql.begin(), ql.end());
        }
    }

    int        bestScore = -Inf;
    Move       bestMove  = Move::none();
    const int  origAlpha = alpha;
    int        moveCount = 0;
    Move       tried[64];
    int        nTried = 0;

    bool sortedAll = false;
    if (kInsertion && stage == 2)
    {
        for (int a = 1; a < n; ++a)
        {
            const Move m = moves[a];
            const int  v = scores[a];
            int        b = a - 1;
            for (; b >= 0 && scores[b] < v; --b)
            {
                moves[b + 1]  = moves[b];
                scores[b + 1] = scores[b];
            }
            moves[b + 1]  = m;
            scores[b + 1] = v;
        }
        sortedAll = true;
    }

    for (int i = 0;; ++i)
    {
        if (i == n)
        {
            if (stage == 1)
            {
                stage = 2;
                const MoveList<QUIETS> ql(pos);
                add(ql.begin(), ql.end());
            }
            if (i == n)
                break;
        }
        if (!sortedAll)
        {
            int bi = i;
            for (int j = i + 1; j < n; ++j)
                if (scores[j] > scores[bi])
                    bi = j;
            std::swap(scores[i], scores[bi]);
            std::swap(moves[i], moves[bi]);
        }

        const Move m          = moves[i];
        const bool capture    = pos.capture(m);
        const bool givesCheck = pos.gives_check(m);
        ++moveCount;

        const int newDepth = depth - 1 + (inCheck ? 1 : 0);

        curMove[ply] = m;
        pos.do_move(m, states[ply], givesCheck, nullptr, nullptr);

        int score;
        if (pvNode && moveCount == 1)
            score = -search(ply + 1, newDepth, -beta, -alpha, true);
        else
        {
            int r = 0;
            if (!exact && depth >= 3 && moveCount > 3 && !inCheck && !givesCheck && !capture)
                r = 1 + (depth >= 6 && moveCount > 10);

            score = -search(ply + 1, newDepth - r, -alpha - 1, -alpha, false);
            if (r > 0 && score > alpha && !aborted)
                score = -search(ply + 1, newDepth, -alpha - 1, -alpha, false);
            if (pvNode && score > alpha && score < beta && !aborted)
                score = -search(ply + 1, newDepth, -beta, -alpha, true);
        }

        pos.undo_move(m);
        if (aborted)
            return 0;

        if (score > bestScore)
        {
            bestScore = score;
            if (score > alpha)
            {
                bestMove = m;
                if (pvNode)
                {
                    pv[ply][0] = m;
                    std::copy_n(pv[ply + 1], pvLen[ply + 1], pv[ply] + 1);
                    pvLen[ply] = pvLen[ply + 1] + 1;
                }
                if (score >= beta)
                    break;
                alpha = score;
            }
        }

        if (m != bestMove && nTried < 64)
            tried[nTried++] = m;
    }

    if (moveCount == 0)
        return inCheck ? MateScore - ply : 0;  // being mated is the goal; stalemate is a draw

    if (bestScore >= beta && bestMove != Move::none())
    {
        const int bonus = std::min(depth * depth, 400);
        auto      bump  = [&](Move m, int b) {
            int& h = hist.h[us][m.from_sq()][m.to_sq()];
            h += b - h * std::abs(b) / 16384;
        };
        bump(bestMove, bonus);
        for (int k = 0; k < nTried; ++k)
            bump(tried[k], -bonus);
        if (!pos.capture(bestMove) && killers[ply][0] != bestMove)
        {
            killers[ply][1] = killers[ply][0];
            killers[ply][0] = bestMove;
            if (kCounter)
                counter[prev.from_sq()][prev.to_sq()] = bestMove;
        }
    }

    const Bound b = bestScore >= beta ? BOUND_LOWER : bestScore > origAlpha ? BOUND_EXACT : BOUND_UPPER;
    tt.store(key, to_tt(bestScore, ply), b, depth, bestMove);
    return bestScore;
}

// One root iteration. rms is ordered best-first. improved is set when a new best move
// was found with an exact score (so a partial iteration can still be used).
int Searcher::root(std::vector<RootEntry>& rms, int depth, std::vector<Move>& pvOut, bool& improved) {
    improved = false;
    int        alpha = -Inf, best = -Inf;
    const bool inCheck = bool(pos.checkers());

    for (std::size_t i = 0; i < rms.size(); ++i)
    {
        const Move m          = rms[i].m;
        const bool givesCheck = pos.gives_check(m);
        const int  newDepth   = depth - 1 + (inCheck ? 1 : 0);

        pos.do_move(m, states[0], givesCheck, nullptr, nullptr);
        int score;
        if (i == 0)
            score = -search(1, newDepth, -Inf, -alpha, true);
        else
        {
            score = -search(1, newDepth, -alpha - 1, -alpha, false);
            if (score > alpha && !aborted)
                score = -search(1, newDepth, -Inf, -alpha, true);
        }
        pos.undo_move(m);
        if (aborted)
            break;

        rms[i].score = score;
        if (score > best)
        {
            best     = score;
            alpha    = std::max(alpha, score);
            improved = true;
            pvOut.assign(1, m);
            pvOut.insert(pvOut.end(), pv[1], pv[1] + pvLen[1]);
        }
    }
    return best;
}

std::string format_score(int score, Report report) {
    if (report == Report::Neutral)
        return "cp 0";
    // Natural: from the engine's own point of view as a normal engine would show it
    // (we get mated = very negative). Inverted: the sign of our goal (being mated = positive).
    const int sign = report == Report::Natural ? 1 : -1;
    if (score >= MateInMax)  // we get mated: what we want
        return "mate " + std::to_string(sign * -((MateScore - score + 1) / 2));
    if (score <= -MateInMax)  // we are forced to mate the opponent
        return "mate " + std::to_string(sign * ((MateScore + score + 1) / 2));
    return "cp " + std::to_string(sign * -score * 100 / int(PawnValue));
}

// Helper thread for Lazy SMP: same root, own position copy, own history, shared table.
struct Helper {
    Helper(const Position& root, const StateInfo& rootState, std::size_t idx, bool c960) :
        s(pos, stopFlag, gTable, hist_for(idx), false) {
        pos.set(root.fen(), c960, &st);
        st = rootState;  // restores the repetition chain, as Stockfish does for its threads
    }

    Position         pos;
    StateInfo        st;
    std::atomic_bool stopFlag{false};
    Searcher         s;
    std::thread      th;
};

constexpr int SkipSize[20]  = {1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4};
constexpr int SkipPhase[20] = {0, 1, 0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 6, 7};

void helper_loop(Helper* h, int idx, std::vector<RootEntry> rms, int maxDepth) {
    for (int depth = 1; depth <= maxDepth && !h->s.aborted; ++depth)
    {
        const int i = (idx - 1) % 20;
        if (((depth + SkipPhase[i]) / SkipSize[i]) % 2)
            continue;  // threads search different depths, so the table fills with varied work
        std::vector<Move> pvLine;
        bool              improved = false;
        h->s.root(rms, depth, pvLine, improved);
        if (h->s.aborted)
            break;
        std::stable_sort(rms.begin(), rms.end(),
                         [](const RootEntry& a, const RootEntry& b) { return a.score > b.score; });
    }
}

}  // namespace


void new_game() {
    gTable.clear();
    for (auto& h : gHists)
        std::memset(h.get(), 0, sizeof(History));
}

int debug_value(Position& pos, int depth, bool reference) {
    init_tables();
    Table t;
    t.resize(4);
    const std::atomic_bool never{false};
    const auto             hist = std::make_unique<History>();
    Searcher               s(pos, never, t, *hist, true);
    if (const char* e = std::getenv("MIS_DBG"))
        s.dbg = std::atoi(e);
    if (const char* e = std::getenv("MIS_PLYCAP"))
        s.plyCap = std::clamp(std::atoi(e), 1, MaxPly);
    return reference ? s.ref(0, depth) : s.search(0, depth, -Inf, Inf, true);
}

Result search(Position&                 pos,
              const std::vector<Move>&  rootMoves,
              const Search::LimitsType& limits,
              const std::atomic_bool&   stop,
              const Config&             cfg) {

    Result res;
    if (rootMoves.empty())
        return res;
    res.best = rootMoves[0];
    if (rootMoves.size() == 1)
        return res;

    init_tables();
    const int nThreads = std::clamp(cfg.threads, 1, 256);
    gTable.resize(std::size_t(std::max(cfg.hashMB, 1)));
    gTable.new_search();
    for (int i = 0; i < nThreads; ++i)
        for (auto& a : hist_for(std::size_t(i)).h)
            for (auto& b : a)
                for (int& h : b)
                    h /= 2;

    const Color     us = pos.side_to_move();
    const TimePoint t0 = limits.startTime ? limits.startTime : now();

    Searcher s(pos, stop, gTable, hist_for(0), false);
    s.nodeLimit = limits.nodes;

    TimePoint soft = 0, hard = 0;
    if (!limits.infinite)
    {
        if (limits.movetime)
            hard = std::max<TimePoint>(limits.movetime - 30, 5);
        else if (limits.use_time_management() && limits.time[us])
        {
            const TimePoint base = limits.time[us] / 30 + limits.inc[us] / 2;
            hard                 = std::min<TimePoint>(base * 3, limits.time[us] * 8 / 10);
            soft                 = base;
        }
    }
    // While pondering the clock does not run. The limits apply from the moment of "ponderhit".
    TimePoint softT0 = t0, softPending = 0;
    if (limits.ponderMode && !limits.infinite && cfg.ponder && cfg.ponder->load())
    {
        s.ponderFlag   = cfg.ponder;
        s.ponderHardMs = hard;
        softPending    = soft;
        soft           = 0;
    }
    else if (hard)
        s.deadline = t0 + hard;

    std::vector<RootEntry> rms;
    for (const Move m : rootMoves)
        rms.push_back({m, -Inf});

    const int maxDepth = limits.depth > 0 ? std::min(limits.depth, MaxPly - 2) : MaxPly - 2;

    std::vector<std::unique_ptr<Helper>> helpers;
    for (int i = 1; i < nThreads; ++i)
    {
        helpers.push_back(
          std::make_unique<Helper>(pos, *pos.state(), std::size_t(i), pos.is_chess960()));
        Helper* h  = helpers.back().get();
        h->th      = std::thread(helper_loop, h, i, rms, maxDepth);
    }
    auto totalNodes = [&]() {
        u64 n = s.nodes;
        for (const auto& h : helpers)
            n += h->s.pubNodes.load(std::memory_order_relaxed);
        return n;
    };

    for (int depth = 1; depth <= maxDepth; ++depth)
    {
        std::vector<Move> pvLine;
        bool              improved = false;
        const int         score    = s.root(rms, depth, pvLine, improved);

        if (improved)
        {
            res.best   = pvLine[0];
            res.ponder = pvLine.size() > 1 ? pvLine[1] : Move::none();
            res.score  = score;
            res.depth  = depth;
            res.matePly = score >= MateInMax ? MateScore - score : 0;
        }
        if (s.aborted)
            break;

        // Best move first, the rest by score.
        std::stable_sort(rms.begin(), rms.end(),
                         [](const RootEntry& a, const RootEntry& b) { return a.score > b.score; });

        const TimePoint elapsed = std::max<TimePoint>(now() - t0, 1);
        std::string     pvs;
        for (const Move m : pvLine)
            pvs += (pvs.empty() ? "" : " ") + UCIEngine::move(m, pos.is_chess960());

        const u64 total = totalNodes();
        sync_cout << "info depth " << depth << " seldepth " << s.maxPly << " multipv 1 score "
                  << format_score(score, cfg.report) << " nodes " << total << " nps " << total * 1000 / u64(elapsed)
                  << " time " << elapsed << " pv " << pvs << sync_endl;

        if (std::abs(score) >= MateInMax && depth >= (score > 0 ? MateScore - score : MateScore + score))
            break;  // forced result proven
        s.check_ponderhit();
        if (s.ponderHitAt && softPending)
        {
            soft        = softPending;
            softT0      = s.ponderHitAt;
            softPending = 0;
        }
        if (soft && (now() - softT0) * 2 > soft)
            break;
    }

    for (auto& h : helpers)
        h->stopFlag.store(true, std::memory_order_relaxed);
    for (auto& h : helpers)
        h->th.join();
    res.nodes = totalNodes();
    return res;
}

}  // namespace Stockfish::Misere
