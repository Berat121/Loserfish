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

#ifndef LOSESEARCH_H_INCLUDED
#define LOSESEARCH_H_INCLUDED

#include <atomic>
#include <vector>

#include "misc.h"
#include "misere.h"
#include "position.h"
#include "search.h"
#include "types.h"

namespace Stockfish::Lose {

struct Result {
    Move best    = Move::none();
    Move ponder  = Move::none();
    int  matePly = 0;  // plies until we are mated on the found line, 0 if no mate line was found
    u64  nodes   = 0;  // adversarial mode only (feeds the bench node count)
};

struct Params {
    int  safetyDepth = 6;      // cooperative mode: plies of the safety search
    int  mateDepth   = 8;      // cooperative mode: maximum plies of the mate search
    int  hashMB      = 16;     // adversarial mode: transposition table size
    const std::atomic_bool* ponder = nullptr;  // set while pondering, cleared by ponderhit
    int  threads     = 1;      // adversarial mode: Lazy SMP search threads
    Misere::Report report = Misere::Report::Neutral;  // how scores are shown in info lines
    bool cooperative = false;  // false: worst-case (misere) opponent, true: opponent mates us if it can
    TimePoint moveOverhead = 30;  // ms reserved per move for GUI and I/O latency
    int       extCap       = 3;   // LoseExtCap: free check evasions per path in the adversarial search
};

// Chooses a move for the engine, whose goal is to lose (get checkmated) and never to win.
// Adversarial (default): the opponent is assumed to play the worst moves against that goal.
// Cooperative: the opponent is assumed to mate us whenever it can.
// rootMoves must contain the legal moves of pos. Runs on the calling thread.
Result choose(Position&                  pos,
              const std::vector<Move>&   rootMoves,
              const Search::LimitsType&  limits,
              const std::atomic_bool&    stop,
              const Params&              params);

// Clears state kept between searches (ucinewgame).
void new_game();

}  // namespace Stockfish::Lose

#endif  // #ifndef LOSESEARCH_H_INCLUDED
