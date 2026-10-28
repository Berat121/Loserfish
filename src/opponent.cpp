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

#include "opponent.h"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <mutex>

#include "movegen.h"

namespace Stockfish::Lose {

namespace {

// Probability that each type plays a mate in one it has (when not every legal move mates).
// A random player plays it with probability mates / legal moves.
constexpr double TakeNormal  = 0.97;
constexpr double TakeRefuser = 0.02;
constexpr double MinProb     = 1e-9;  // keeps every hypothesis alive numerically

// Older plies add little and a very long game must not cost much time.
constexpr std::size_t MaxAnalyzedPlies = 300;

struct History {
    std::string       fen;
    bool              chess960 = false;
    std::vector<Move> moves;
};

std::mutex gMutex;
History    gHistory;

void update(double (&post)[3], bool taken, int mates, std::size_t legal) {
    const double pRandom = double(mates) / double(legal);
    const double like[3] = {taken ? TakeNormal : 1.0 - TakeNormal,
                            taken ? TakeRefuser : 1.0 - TakeRefuser,
                            taken ? pRandom : 1.0 - pRandom};
    double       sum     = 0.0;
    for (int t = 0; t < 3; ++t)
    {
        post[t] = std::max(post[t] * like[t], MinProb);
        sum += post[t];
    }
    for (double& p : post)
        p /= sum;
}

}  // namespace

void set_history(const std::string& fen, bool chess960, const std::vector<Move>& moves) {
    std::lock_guard<std::mutex> lock(gMutex);
    gHistory.fen      = fen;
    gHistory.chess960 = chess960;
    gHistory.moves    = moves;
}

void clear_history() {
    std::lock_guard<std::mutex> lock(gMutex);
    gHistory = History();
}

bool is_mating_move(Position& pos, Move m) {
    if (!pos.gives_check(m))
        return false;
    StateInfo st;
    pos.do_move(m, st, nullptr);
    const bool mate = MoveList<ALL>(pos).size() == 0;  // in check and no legal reply
    pos.undo_move(m);
    return mate;
}

double mate_chance(Position& pos, Move m) {
    StateInfo st;
    pos.do_move(m, st, nullptr);
    const MoveList<ALL> replies(pos);
    int                 mates = 0;
    for (const auto& e : replies)
        mates += is_mating_move(pos, Move(e));
    const std::size_t n = replies.size();
    pos.undo_move(m);
    return n ? double(mates) / double(n) : 0.0;
}

OpponentModel infer_opponent(const Position& root) {
    OpponentModel model;

    History h;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        h = gHistory;
    }
    if (h.moves.empty())
        return model;

    Position              p;
    std::deque<StateInfo> states(1);
    if (p.set(h.fen, h.chess960, &states.back()))
        return model;

    const Color       opp   = ~root.side_to_move();
    const std::size_t first = h.moves.size() > MaxAnalyzedPlies ? h.moves.size() - MaxAnalyzedPlies : 0;

    double post[3] = {model.normal, model.refuser, model.random};
    int    chances = 0, taken = 0;

    for (std::size_t i = 0; i < h.moves.size(); ++i)
    {
        const Move played = h.moves[i];

        if (i >= first && p.side_to_move() == opp)
        {
            const MoveList<ALL> legal(p);
            int                 mates = 0;
            for (const auto& e : legal)
                mates += is_mating_move(p, Move(e));

            // With no mate in one there is nothing to learn; when every move mates there was no choice.
            if (mates > 0 && std::size_t(mates) < legal.size())
            {
                const bool took = is_mating_move(p, played);
                update(post, took, mates, legal.size());
                ++chances;
                taken += took;
            }
        }

        states.emplace_back();
        p.do_move(played, states.back(), nullptr);
    }

    // The recorded game must end in the position we are asked about.
    if (p.key() != root.key())
        return OpponentModel();

    model.normal  = post[0];
    model.refuser = post[1];
    model.random  = post[2];
    model.chances = chances;
    model.taken   = taken;
    return model;
}

}  // namespace Stockfish::Lose
