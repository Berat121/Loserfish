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

#include "losesearch.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstddef>
#include <iostream>
#include <string>

#include "attacks.h"
#include "bitboard.h"
#include "misere.h"
#include "movegen.h"
#include "uci.h"

namespace Stockfish::Lose {

namespace {

constexpr int MaxLosePly = 32;  // hard bound for every array and for the recursion depth
constexpr int MateScore  = 30000;
constexpr int DrawScore  = -5000;
constexpr int NoScore    = -1000000;  // "no mate line found" (below every real score)

enum class End {
    None,
    UsMated,    // we are checkmated: the outcome we want
    ThemMated,  // the opponent is checkmated: forbidden
    Draw        // stalemate or draw by rule: not wanted
};

struct Ctx {
    Ctx(Position& p, Color c, const std::atomic_bool& s) :
        pos(p),
        us(c),
        stop(s) {}

    Position&               pos;
    Color                   us;
    const std::atomic_bool& stop;

    StateInfo states[MaxLosePly + 2];
    Move      pv[MaxLosePly + 2][MaxLosePly + 2];
    int       pvLen[MaxLosePly + 2] = {};

    TimePoint deadline    = 0;  // 0: no time limit
    // While pondering the clock does not run; the budget starts at "ponderhit".
    const std::atomic_bool* ponderFlag   = nullptr;
    TimePoint               ponderBudget = 0;
    u64       nodes       = 0;
    u64       nodeLimit   = 0;  // 0: no node limit
    bool      aborted     = false;
    int       bestMatePly = INT_MAX;
};

bool insufficient_material(const Position& pos) {
    return !pos.count<PAWN>() && !pos.count<ROOK>() && !pos.count<QUEEN>()
        && pos.count<KNIGHT>() + pos.count<BISHOP>() <= 1;
}

bool check_abort(Ctx& c) {
    if ((c.nodes & 1023) == 0)
    {
        if (c.ponderFlag && !c.ponderFlag->load(std::memory_order_relaxed))
        {
            c.ponderFlag = nullptr;
            if (c.ponderBudget)
                c.deadline = now() + c.ponderBudget;
        }
        if (c.stop.load(std::memory_order_relaxed))
            c.aborted = true;
        else if (c.deadline && now() >= c.deadline)
            c.aborted = true;
    }
    if (c.nodeLimit && c.nodes >= c.nodeLimit)
        c.aborted = true;
    return c.aborted;
}

// strict: only draws that cannot be avoided by the side to move count
// (stalemate, 50-move rule, threefold repetition, insufficient material).
// Non-strict additionally treats a repetition inside the search as a draw.
End classify(const Position& pos, Color us, int ply, std::size_t legalCount, bool strict) {
    if (legalCount == 0)
    {
        if (!pos.checkers())
            return End::Draw;
        return pos.side_to_move() == us ? End::UsMated : End::ThemMated;
    }
    if (insufficient_material(pos))
        return End::Draw;
    if (strict)
    {
        if (pos.rule50_count() > 99 || pos.state()->repetition < 0)
            return End::Draw;
    }
    else if (pos.is_draw(ply))
        return End::Draw;
    return End::None;
}

// Heuristic "how lost are we" score from our point of view (higher is better
// for the goal of losing): material deficit and an exposed king.
int leaf_score(const Position& pos, Color us) {
    const Color them = ~us;
    int         score =
      -(PawnValue * (pos.count<PAWN>(us) - pos.count<PAWN>(them)) + int(pos.non_pawn_material(us))
        - int(pos.non_pawn_material(them)));

    const Square   ksq  = pos.square<KING>(us);
    const Bitboard ring = Attacks::attacks_bb(KING, ksq, Bitboard(0)) | square_bb(ksq);
    int            attacked = 0, flight = 0;

    Bitboard b = ring;
    while (b)
    {
        const Square s   = pop_lsb(b);
        const bool   hit = (pos.attackers_to(s) & pos.pieces(them)) != 0;
        attacked += hit;
        if (s != ksq && !(pos.pieces(us) & s) && !hit)
            ++flight;
    }
    score += 40 * attacked - 30 * flight;

    if (pos.checkers() && pos.side_to_move() == us)
        score += 100;

    return score;
}

// ---------------------------------------------------------------------------
// Safety: can the opponent force a result that is not our loss?
// AND/OR search. The opponent chooses (OR), we choose among our replies (AND
// over all of them: it is forced only if every reply fails).
// ---------------------------------------------------------------------------

bool opp_can_force(Ctx& c, int ply, int d, const MoveList<ALL>& ml);

// Position: we are to move, not terminal. d >= 1 plies remain (our reply included).
bool we_cannot_avoid(Ctx& c, int ply, int d, const MoveList<ALL>& ml) {
    ++c.nodes;
    if (check_abort(c) || ply >= MaxLosePly)
        return false;

    Position& pos = c.pos;

    // Quiet replies first: a checking reply is the only one that can mate.
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& e : ml)
        {
            const Move m = e;
            if (pos.gives_check(m) != (pass == 1))
                continue;

            pos.do_move(m, c.states[ply], nullptr);
            const MoveList<ALL> child(pos);
            const End             fin = classify(pos, c.us, ply + 1, child.size(), true);

            bool forced;
            if (fin == End::Draw || fin == End::ThemMated)
                forced = true;
            else if (fin == End::UsMated)
                forced = false;
            else
                forced = d >= 2 && opp_can_force(c, ply + 1, d - 1, child);
            pos.undo_move(m);

            if (c.aborted)
                return false;
            if (!forced)
                return false;  // we have a reply that keeps the loss reachable
        }
    return true;
}

// Position: opponent to move, not terminal. d >= 1 plies remain (their move included).
bool opp_can_force(Ctx& c, int ply, int d, const MoveList<ALL>& ml) {
    ++c.nodes;
    if (check_abort(c) || ply >= MaxLosePly)
        return false;

    Position& pos = c.pos;

    for (const auto& e : ml)
    {
        const Move m = e;
        pos.do_move(m, c.states[ply], nullptr);
        const MoveList<ALL> child(pos);
        const End             fin = classify(pos, c.us, ply + 1, child.size(), true);

        bool forced = false;
        if (fin == End::Draw)
            forced = true;
        else if (fin == End::None && d >= 2)
            forced = we_cannot_avoid(c, ply + 1, d - 1, child);
        pos.undo_move(m);

        if (c.aborted)
            return false;
        if (forced)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Goal: cooperative (max-max) search for the shortest line ending with our mate.
// Scores: MateScore - ply when we are mated at ply (higher is better), leaf
// heuristic otherwise. MateOnly searches only for mate lines and is much faster.
// ---------------------------------------------------------------------------

template<bool MateOnly>
int coop(Ctx& c, int ply, int depthLeft) {
    ++c.nodes;
    c.pvLen[ply] = 0;
    if (check_abort(c))
        return NoScore;

    Position&             pos = c.pos;
    const MoveList<ALL> ml(pos);

    switch (classify(pos, c.us, ply, ml.size(), false))
    {
    case End::UsMated :
        c.bestMatePly = std::min(c.bestMatePly, ply);
        return MateScore - ply;
    case End::ThemMated :
        return MateOnly ? NoScore : -MateScore + ply;
    case End::Draw :
        return MateOnly ? NoScore : DrawScore;
    case End::None :
        break;
    }

    if (depthLeft <= 0 || ply >= MaxLosePly - 1)
        return MateOnly ? NoScore : leaf_score(pos, c.us);

    const bool usToMove = pos.side_to_move() == c.us;
    // We can only be mated by the opponent's move: earliest possible mate ply below this node.
    const int earliest = usToMove ? ply + 2 : ply + 1;

    // A shorter mate is already known, nothing below can beat it.
    if (earliest >= c.bestMatePly)
        return MateOnly ? NoScore : leaf_score(pos, c.us);

    if (MateOnly && earliest - ply > depthLeft)
        return NoScore;

    const bool lastOppPly = MateOnly && !usToMove && depthLeft == 1;

    int best = NoScore;
    for (const auto& e : ml)
    {
        const Move m = e;
        if (lastOppPly && !pos.gives_check(m))
            continue;

        pos.do_move(m, c.states[ply], nullptr);
        const int v = coop<MateOnly>(c, ply + 1, depthLeft - 1);
        pos.undo_move(m);

        if (c.aborted)
            return NoScore;

        if (v > best)
        {
            best = v;
            assert(c.pvLen[ply + 1] + 1 <= MaxLosePly);
            c.pv[ply][0] = m;
            std::copy_n(c.pv[ply + 1], c.pvLen[ply + 1], c.pv[ply] + 1);
            c.pvLen[ply] = c.pvLen[ply + 1] + 1;

            if (best == MateScore - earliest)  // cannot do better at this node
                break;
        }
    }
    return best;
}

std::string pv_string(Position& pos, const std::vector<Move>& line) {
    std::string s;
    for (const Move m : line)
    {
        if (!s.empty())
            s += ' ';
        s += UCIEngine::move(m, pos.is_chess960());
    }
    return s;
}

}  // namespace


static Result choose_cooperative(Position&                 pos,
                                 const std::vector<Move>&  rootMoves,
                                 const Search::LimitsType& limits,
                                 const std::atomic_bool&   stop,
                                 int                       safetyDepth,
                                 int                       mateDepth,
                                 Misere::Report            report,
                                 const std::atomic_bool*   ponder,
                                 TimePoint                 moveOverhead) {

    Result res;
    if (rootMoves.empty())
        return res;
    res.best = rootMoves[0];

    safetyDepth = std::clamp(safetyDepth, 1, 12);
    mateDepth   = std::clamp(mateDepth, 2, 16);
    if (limits.depth > 0)
        mateDepth = std::min(mateDepth, std::max(2, 2 * limits.depth));

    const Color us    = pos.side_to_move();
    const TimePoint t0 = limits.startTime ? limits.startTime : now();

    Ctx c(pos, us, stop);
    c.nodeLimit = limits.nodes;

    const Misere::TimePlan plan   = Misere::plan_time(limits, us, moveOverhead);
    const TimePoint        budget = limits.movetime ? plan.hard : plan.base;
    if (limits.ponderMode && !limits.infinite && ponder && ponder->load())
    {
        c.ponderFlag   = ponder;
        c.ponderBudget = budget;
    }
    else if (budget && !limits.infinite)
        c.deadline = t0 + budget;

    auto elapsed = [&]() { return std::max<TimePoint>(now() - t0, 1); };

    // Score text for info lines; Neutral never shows an adjudicable score.
    auto cpText = [&](int natural) {
        return std::to_string(report == Misere::Report::Neutral   ? 0
                              : report == Misere::Report::Natural ? natural
                                                                  : -natural);
    };
    auto mateText = [&](int n) {
        return report == Misere::Report::Neutral ? std::string("cp 0")
             : report == Misere::Report::Natural ? "mate -" + std::to_string(n)
                                                 : "mate " + std::to_string(n);
    };

    // 1. Classify root moves.
    std::vector<Move> safe, unsafe, drawing, winning;
    for (const Move m : rootMoves)
    {
        pos.do_move(m, c.states[0], nullptr);
        const MoveList<ALL> child(pos);
        const End             fin = classify(pos, us, 1, child.size(), true);

        if (fin == End::ThemMated)
            winning.push_back(m);
        else if (fin == End::Draw)
            drawing.push_back(m);
        else if (opp_can_force(c, 1, safetyDepth, child) && !c.aborted)
            unsafe.push_back(m);
        else if (c.aborted)
        {
            pos.undo_move(m);
            break;
        }
        else
            safe.push_back(m);
        pos.undo_move(m);
    }
    c.aborted = false;  // a timeout while classifying must not stop the fallbacks

    const std::vector<Move>* candidates = &safe;
    const char*              tier       = "safe";
    if (candidates->empty())
    {
        candidates = &unsafe;
        tier       = "unsafe (opponent can force a non-loss)";
    }
    if (candidates->empty())
    {
        candidates = &drawing;
        tier       = "drawing";
    }
    if (candidates->empty())
    {
        candidates = &winning;
        tier       = "winning (only legal moves mate)";
    }
    if (candidates->empty())  // classification was cut short: fall back to all moves
    {
        candidates = &rootMoves;
        tier       = "all moves (classification interrupted)";
    }

    sync_cout << "info string lose-mode candidates " << candidates->size() << "/" << rootMoves.size()
              << " tier " << tier << sync_endl;

    res.best = (*candidates)[0];
    if (candidates->size() == 1)
        return res;

    // Picks the move after which the cooperative search reaches the most lost-looking position.
    auto headTowardsLoss = [&](int fromDepth, int toDepth, bool print) {
        for (int hd = fromDepth; hd <= toDepth; ++hd)
        {
            c.bestMatePly = INT_MAX;
            int               bestV = NoScore - 1;
            Move              bestM = Move::none();
            std::vector<Move> bestLine;

            for (const Move m : *candidates)
            {
                pos.do_move(m, c.states[0], nullptr);
                const int v = coop<false>(c, 1, hd - 1);
                if (v > bestV && !c.aborted)
                {
                    bestV = v;
                    bestM = m;
                    bestLine.assign(1, m);
                    bestLine.insert(bestLine.end(), c.pv[1], c.pv[1] + c.pvLen[1]);
                }
                pos.undo_move(m);
                if (c.aborted)
                    break;
            }
            if (c.aborted || bestM == Move::none())
                return;

            res.best   = bestM;
            res.ponder = bestLine.size() > 1 ? bestLine[1] : Move::none();

            if (print)
                sync_cout << "info depth " << hd << " seldepth " << hd << " multipv 1 score cp "
                          << cpText(-bestV) << " nodes " << c.nodes << " nps "
                          << c.nodes * 1000 / u64(elapsed()) << " time " << elapsed() << " pv "
                          << pv_string(pos, bestLine) << sync_endl;
        }
    };

    // Cheap first answer so that an interrupted search still returns a sensible move.
    headTowardsLoss(2, 2, false);
    if (c.aborted)
        return res;

    // 2. Shortest cooperative mate of ourselves.
    std::vector<Move> order = *candidates;
    for (int d = 2; d <= mateDepth; d += 2)
    {
        c.bestMatePly = INT_MAX;
        int               bestV = NoScore;
        Move              bestM = Move::none();
        std::vector<Move> bestLine;

        for (const Move m : order)
        {
            pos.do_move(m, c.states[0], nullptr);
            const int v = coop<true>(c, 1, d - 1);
            if (v > bestV && !c.aborted)
            {
                bestV = v;
                bestM = m;
                bestLine.assign(1, m);
                bestLine.insert(bestLine.end(), c.pv[1], c.pv[1] + c.pvLen[1]);
            }
            pos.undo_move(m);
            if (c.aborted)
                break;
        }
        if (c.aborted)
            return res;

        if (bestV > NoScore)
        {
            res.best    = bestM;
            res.matePly = MateScore - bestV;
            res.ponder  = bestLine.size() > 1 ? bestLine[1] : Move::none();

            sync_cout << "info depth " << d << " seldepth " << d << " multipv 1 score "
                      << mateText((res.matePly + 1) / 2) << " nodes " << c.nodes << " nps "
                      << c.nodes * 1000 / u64(elapsed()) << " time " << elapsed() << " pv "
                      << pv_string(pos, bestLine) << sync_endl;
            return res;
        }
        sync_cout << "info depth " << d << " nodes " << c.nodes << " time " << elapsed()
                  << sync_endl;
    }

    // 3. No mate within the horizon: head towards a lost position.
    headTowardsLoss(1, 4, true);
    return res;
}

Result choose(Position&                 pos,
              const std::vector<Move>&  rootMoves,
              const Search::LimitsType& limits,
              const std::atomic_bool&   stop,
              const Params&             params) {

    if (params.cooperative)
        return choose_cooperative(pos, rootMoves, limits, stop, params.safetyDepth,
                                  params.mateDepth, params.report, params.ponder,
                                  params.moveOverhead);

    Misere::Config cfg;
    cfg.hashMB  = params.hashMB;
    cfg.threads = params.threads;
    cfg.ponder  = params.ponder;
    cfg.report  = params.report;
    cfg.moveOverhead = params.moveOverhead;
    const Misere::Result r = Misere::search(pos, rootMoves, limits, stop, cfg);

    Result res;
    res.best    = r.best;
    res.ponder  = r.ponder;
    res.nodes   = r.nodes;
    res.matePly = r.matePly;
    return res;
}

void new_game() { Misere::new_game(); }

}  // namespace Stockfish::Lose
