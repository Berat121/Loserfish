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

#ifndef MISERE_H_INCLUDED
#define MISERE_H_INCLUDED

#include <atomic>
#include <vector>

#include "misc.h"
#include "position.h"
#include "search.h"
#include "types.h"

namespace Stockfish::Misere {

// Misere chess: the side that gets checkmated wins. Every node is solved as a
// zero-sum game in which the opponent plays the worst possible moves against
// our goal (it avoids mating us, and tries to force us to mate it).
// Scores are from the point of view of the side to move; higher is better,
// +MateScore - ply means "the side to move gets mated at ply" (a win).

struct Result {
    Move best    = Move::none();
    Move ponder  = Move::none();
    int  score   = 0;  // misere score from our point of view
    int  depth   = 0;
    int  matePly = 0;  // plies until we are mated on the PV when a forced win was found, else 0
    u64  nodes   = 0;  // all threads; helper counts lag by up to 1023 nodes
};

// How scores are shown in "info" lines. GUIs adjudicate on them, and for this engine
// a "bad" score is the goal, so the default hides it.
enum class Report {
    Neutral,   // always cp 0 (GUIs never adjudicate)
    Natural,   // as a normal engine: being mated shows as a negative score
    Inverted   // sign of our goal: being mated shows as a positive score
};

struct Config {
    const std::atomic_bool* ponder = nullptr;  // main_manager()->ponder; false after "ponderhit"
    int    hashMB  = 16;
    int    threads = 1;
    Report report  = Report::Neutral;
};

Result search(Position&                 pos,
              const std::vector<Move>&  rootMoves,
              const Search::LimitsType& limits,
              const std::atomic_bool&   stop,
              const Config&             cfg);

void new_game();

// Test hooks: root value of pos at a fixed depth with pruning disabled (exact mode),
// computed either by the optimized search or by plain minimax.
int debug_value(Position& pos, int depth, bool reference);

}  // namespace Stockfish::Misere

#endif  // #ifndef MISERE_H_INCLUDED
