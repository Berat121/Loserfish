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

#ifndef OPPONENT_H_INCLUDED
#define OPPONENT_H_INCLUDED

#include <string>
#include <vector>

#include "position.h"
#include "types.h"

namespace Stockfish::Lose {

// Who is on the other side of the board, as far as the game so far shows.
// The three probabilities sum to 1; the defaults are the prior (no evidence yet).
struct OpponentModel {
    double normal  = 0.6;  // plays to win: mates us whenever it can
    double refuser = 0.2;  // never mates us unless every legal move does
    double random  = 0.2;  // plays any legal move with equal probability
    int    chances = 0;    // mates in one the opponent had (positions where every move mates do not count)
    int    taken   = 0;    // of those, how many it played
};

// Remembers the game of the last "position" command: start FEN and the moves played from it.
void set_history(const std::string& fen, bool chess960, const std::vector<Move>& moves);
void clear_history();

// Bayesian update of the opponent type from every mate in one the opponent had and did or did not
// play. `root` is the position to move for us; the opponent is the other side. Returns the prior
// when there is no history or the recorded game does not end in `root`.
OpponentModel infer_opponent(const Position& root);

// True when m checkmates the opponent of the side to move.
bool is_mating_move(Position& pos, Move m);

// Probability that a uniformly random reply of the opponent checkmates us after we play m.
// 0 when m ends the game. pos is restored.
double mate_chance(Position& pos, Move m);

}  // namespace Stockfish::Lose

#endif  // #ifndef OPPONENT_H_INCLUDED
