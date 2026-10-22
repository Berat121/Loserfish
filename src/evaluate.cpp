/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "evaluate.h"

#include <algorithm>
#include <cassert>
#include <iomanip>
#include <sstream>

#include "position.h"
#include "types.h"
#include "uci.h"

namespace Stockfish {

Value Eval::evaluate(const Position& pos, int /*optimism*/) {

    assert(!pos.checkers());

    const Color c = pos.side_to_move();
    int         v = PawnValue * (pos.count<PAWN>(c) - pos.count<PAWN>(~c)) + pos.non_pawn_material(c)
            - pos.non_pawn_material(~c);

    v -= v * pos.rule50_count() / 189;

    return Value(std::clamp(v, int(VALUE_TB_LOSS_IN_MAX_PLY) + 1, int(VALUE_TB_WIN_IN_MAX_PLY) - 1));
}

// Trace scores are from white's point of view
std::string Eval::trace(Position& pos) {

    if (pos.checkers())
        return "Final evaluation: none (in check)";

    Value v = evaluate(pos, VALUE_ZERO);
    v       = pos.side_to_move() == WHITE ? v : -v;

    std::stringstream ss;
    ss << std::showpoint << std::showpos << std::fixed << std::setprecision(2) << std::setw(15);
    ss << "\nMaterial evaluation   " << 0.01 * UCIEngine::to_cp(v, pos) << " (white side)\n";
    return ss.str();
}

}  // namespace Stockfish
