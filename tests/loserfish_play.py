import chess, chess.engine, random, sys, multiprocessing as mp
from collections import Counter

import os
ENGINE = os.environ.get("LOSERFISH", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "loserfish"))
OPP_ENGINE = os.environ.get("OPPENGINE", ENGINE)  # optional different binary for the opponent
# usage: play_games.py N OPP [MODE] [MOVETIME]
#   OPP  = normal | adv | coop      (what Loserfish plays against)
#   MODE = adversarial | cooperative | adaptive (Loserfish's own assumption, default adversarial)

def play(args):
    seed, opp_kind, mode, mt = args
    rnd = random.Random(seed)
    board = chess.Board()
    for _ in range(rnd.choice([0, 2, 4, 6])):
        board.push(rnd.choice(list(board.legal_moves)))
        if board.is_game_over():
            break
    me_color = chess.WHITE if seed % 2 == 0 else chess.BLACK
    me = chess.engine.SimpleEngine.popen_uci(ENGINE)
    opp = chess.engine.SimpleEngine.popen_uci(OPP_ENGINE)
    me.configure({"LoseMode": True, "LoseOpponent": mode})
    if opp_kind == "normal":
        opp.configure({"LoseMode": False})
    else:
        opp.configure({"LoseMode": True, "LoseOpponent": "adversarial" if opp_kind == "adv" else "cooperative"})
    try:
        while not board.is_game_over(claim_draw=True) and board.fullmove_number < 150:
            eng = me if board.turn == me_color else opp
            r = eng.play(board, chess.engine.Limit(time=mt))
            assert r.move in board.legal_moves, f"illegal {r.move} in {board.fen()}"
            board.push(r.move)
        out = board.outcome(claim_draw=True)
        if out is None:
            res = "unfinished"
        elif out.winner is None:
            res = "draw:" + out.termination.name
        elif out.winner == me_color:
            res = "WON(bad)"
        else:
            res = "lost(good)"
        return seed, res, len(board.move_stack)
    finally:
        me.quit(); opp.quit()

if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 12
    opp = sys.argv[2] if len(sys.argv) > 2 else "normal"
    mode = sys.argv[3] if len(sys.argv) > 3 else "adversarial"
    mt = float(sys.argv[4]) if len(sys.argv) > 4 else 0.1
    with mp.Pool(2) as p:
        results = p.map(play, [(i, opp, mode, mt) for i in range(n)])
    print(Counter(r for _, r, _ in results))
    for r in results:
        print(r)
