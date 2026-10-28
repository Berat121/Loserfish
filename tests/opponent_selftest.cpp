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

// Unit test of the opponent model: history replay, Bayesian update, mate chance.
//   cmake --build build --target opponent_selftest && build/opponent_selftest

#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "attacks.h"
#include "opponent.h"
#include "position.h"
#include "uci.h"

using namespace Stockfish;

namespace {

int failures = 0;
int checks   = 0;

#define CHECK(cond) \
    do \
    { \
        ++checks; \
        if (!(cond)) \
        { \
            ++failures; \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
        } \
    } while (0)

bool near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) < eps; }

// A position reached from a FEN by a list of UCI moves.
struct Game {
    std::string           fen;
    std::vector<Move>     moves;
    Position              pos;
    std::deque<StateInfo> states;

    Game(const std::string& f, const std::vector<std::string>& uci) :
        fen(f),
        states(1) {
        pos.set(fen, false, &states.back());
        for (const auto& s : uci)
        {
            const Move m = UCIEngine::to_move(pos, s);
            moves.push_back(m);
            states.emplace_back();
            pos.do_move(m, states.back(), nullptr);
        }
    }
    void record() const { Lose::set_history(fen, false, moves); }
};

// White to move can mate with Ra8 (1 of 17 legal moves).
const char* BackRank = "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1";

// Independent reference of the update for one observation (prior 0.6 / 0.2 / 0.2).
void reference(bool taken, int mates, int legal, double (&out)[3]) {
    const double pr   = double(mates) / legal;
    const double w[3] = {0.6 * (taken ? 0.97 : 0.03), 0.2 * (taken ? 0.02 : 0.98),
                         0.2 * (taken ? pr : 1.0 - pr)};
    const double sum  = w[0] + w[1] + w[2];
    for (int i = 0; i < 3; ++i)
        out[i] = w[i] / sum;
}

}  // namespace

int main() {
    Attacks::init();
    Position::init();

    // No history: the prior.
    {
        Lose::clear_history();
        Game g(BackRank, {"g1h1"});
        const Lose::OpponentModel m = Lose::infer_opponent(g.pos);
        CHECK(near(m.normal, 0.6) && near(m.refuser, 0.2) && near(m.random, 0.2));
        CHECK(m.chances == 0);
    }

    // The opponent (White) had Ra8# and played a king move: it declined.
    double declined[3];
    {
        Game g(BackRank, {"g1h1"});
        g.record();
        const Lose::OpponentModel m = Lose::infer_opponent(g.pos);
        double                    ref[3];
        reference(false, 1, 17, ref);
        CHECK(near(m.normal, ref[0]) && near(m.refuser, ref[1]) && near(m.random, ref[2]));
        CHECK(m.chances == 1 && m.taken == 0);
        CHECK(m.normal < 0.06 && m.refuser > 0.45);
        declined[0] = m.normal;
        declined[1] = m.refuser;
        declined[2] = m.random;
    }

    // The opponent took the mate.
    {
        Game g(BackRank, {"a1a8"});
        g.record();
        const Lose::OpponentModel m = Lose::infer_opponent(g.pos);
        double                    ref[3];
        reference(true, 1, 17, ref);
        CHECK(near(m.normal, ref[0]) && near(m.refuser, ref[1]) && near(m.random, ref[2]));
        CHECK(m.chances == 1 && m.taken == 1);
        CHECK(m.normal > 0.95);
    }

    // Two declined mates are stronger evidence than one.
    {
        Game g(BackRank, {"g1h1", "g8h8", "h1g1"});
        g.record();
        const Lose::OpponentModel m = Lose::infer_opponent(g.pos);
        CHECK(m.chances == 2 && m.taken == 0);
        CHECK(m.normal < declined[0] && m.refuser > declined[1]);
        CHECK(near(m.normal + m.refuser + m.random, 1.0));
    }

    // Every legal move mates: the opponent had no choice, nothing is learned.
    {
        Game g("K7/1q3Q2/k4P2/8/8/1N4P1/8/8 w - - 0 1", {"f7b7"});
        g.record();
        const Lose::OpponentModel m = Lose::infer_opponent(g.pos);
        CHECK(m.chances == 0);
        CHECK(near(m.normal, 0.6) && near(m.refuser, 0.2) && near(m.random, 0.2));
    }

    // Our own declined mate says nothing about the opponent.
    {
        Game g(BackRank, {"g1h1", "g8h8"});  // White (us at the root) declined, Black replied
        g.record();
        const Lose::OpponentModel m = Lose::infer_opponent(g.pos);
        CHECK(m.chances == 0);
        CHECK(near(m.normal, 0.6));
    }

    // A recorded game that does not end in the root position is ignored.
    {
        Game g(BackRank, {"g1h1"});
        g.record();
        Game other(BackRank, {});
        const Lose::OpponentModel m = Lose::infer_opponent(other.pos);
        CHECK(m.chances == 0 && near(m.normal, 0.6));
    }

    // Mate detection and mate chance.
    {
        Game g(BackRank, {});
        CHECK(Lose::is_mating_move(g.pos, UCIEngine::to_move(g.pos, "a1a8")));
        CHECK(!Lose::is_mating_move(g.pos, UCIEngine::to_move(g.pos, "g1h1")));

        Game b("6k1/5ppp/8/8/8/8/8/R5K1 b - - 0 1", {});
        const Key before = b.pos.key();
        // After Kh8 the opponent has 17 legal moves and Ra8 is the only mate.
        CHECK(near(Lose::mate_chance(b.pos, UCIEngine::to_move(b.pos, "g8h8")), 1.0 / 17.0));
        // After h6 the king can run to h7: no reply mates.
        CHECK(near(Lose::mate_chance(b.pos, UCIEngine::to_move(b.pos, "h7h6")), 0.0));
        CHECK(b.pos.key() == before);
    }

    // A very long game is cheap to replay and analyse.
    {
        std::vector<std::string> shuffle;
        for (int i = 0; i < 500; ++i)
            for (const char* s : {"e1e2", "e8e7", "e2e1", "e7e8"})
                shuffle.push_back(s);
        Game g("4k3/8/8/8/8/8/8/4K3 w - - 0 1", shuffle);
        g.record();
        const auto t0 = std::chrono::steady_clock::now();
        const Lose::OpponentModel m = Lose::infer_opponent(g.pos);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
        CHECK(m.chances == 0 && near(m.normal, 0.6));
        CHECK(ms < 500);
    }

    Lose::clear_history();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
