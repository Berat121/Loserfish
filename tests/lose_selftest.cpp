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

// Validates the optimized misere search against plain minimax on many positions.
// Build: see tests/README.md. Reads FEN lines from stdin: "<fen>" per line.
#include <cstdlib>
#include <iostream>
#include <string>

#include "../src/attacks.h"
#include "../src/misere.h"
#include "../src/position.h"

using namespace Stockfish;

int main(int argc, char** argv) {
    const int depth = argc > 1 ? std::atoi(argv[1]) : 3;
    Attacks::init();
    Position::init();

    // EXPECT_ZERO=1: every input position must evaluate to exactly 0 (use with stalemate positions
    // at depth 0: a stalemate is a draw, not an evaluation).
    // EXPECT_VALUE=<n> generalizes it: every position must evaluate to exactly n.
    const char* expectEnv  = std::getenv("EXPECT_VALUE");
    const bool  expectZero = expectEnv || std::getenv("EXPECT_ZERO") != nullptr;
    const int   expected   = expectEnv ? std::atoi(expectEnv) : 0;

    std::string fen;
    int         total = 0, bad = 0;
    while (std::getline(std::cin, fen))
    {
        if (fen.empty())
            continue;
        Position  pos;
        StateInfo st;
        pos.set(fen, false, &st);
        const int a = Misere::debug_value(pos, depth, false);
        const int b = Misere::debug_value(pos, depth, true);
        ++total;
        if (expectZero && (a != expected || b != expected))
        {
            ++bad;
            std::cout << "NOT EXPECTED (" << expected << ") depth " << depth << " fast " << a << " ref " << b << " fen " << fen << "\n";
        }
        else if (a != b)
        {
            ++bad;
            std::cout << "MISMATCH depth " << depth << " fast " << a << " ref " << b << " fen " << fen << "\n";
        }
    }
    std::cout << "checked " << total << " positions at depth " << depth << ", mismatches " << bad << "\n";
    return bad ? 1 : 0;
}
