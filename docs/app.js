import { Chess } from './vendor/chess.js';

const $ = (id) => document.getElementById(id);
const pieceClass = (color, type) => 'p ' + color + type.toUpperCase();
const FILES = 'abcdefgh';
const SETUP_TIMEOUT_MS = 30000;

let M = null;
let waiter = null;
let searchPromise = null;
let game = new Chess();
let ucis = [];
let human = 'w';
let movetime = 1000;
let gen = 0;
let selected = null;
let lastMove = null;
let ready = false;
let engineError = null;

function onLine(raw) {
  const line = String(raw);
  if (line.startsWith('info depth ')) {
    const d = /\bdepth (\d+)/.exec(line);
    const n = /\bnps (\d+)/.exec(line);
    if (d) $('info').textContent = 'depth ' + d[1] + (n ? ', ' + (Number(n[1]) / 1e6).toFixed(2) + ' Mnps' : '');
  }
  if (waiter && waiter.re.test(line)) {
    const w = waiter;
    waiter = null;
    w.resolve(line);
  }
}

function expect(re, ms) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      waiter = null;
      reject(new Error('Engine timeout: ' + re));
    }, ms);
    waiter = { re, resolve: (l) => { clearTimeout(timer); resolve(l); } };
  });
}

function send(line) {
  M.ccall('loserfish_cmd', null, ['string'], [line]);
}

async function sync() {
  const p = expect(/^readyok/, SETUP_TIMEOUT_MS);
  send('isready');
  await p;
}

async function initEngine() {
  M = await globalThis.Loserfish({ print: onLine, printErr: (l) => console.warn(l), arguments: [] });
  const p = expect(/^uciok/, SETUP_TIMEOUT_MS);
  send('uci');
  await p;
  await sync();
}

function searchBest(list, ms) {
  const p = expect(/^bestmove /, ms + 20000);
  searchPromise = p;
  const clear = () => { if (searchPromise === p) searchPromise = null; };
  p.then(clear, clear);
  send(list.length ? 'position startpos moves ' + list.join(' ') : 'position startpos');
  send('go movetime ' + ms);
  return p;
}

async function stopSearch() {
  const p = searchPromise;
  if (!p) return;
  send('stop');
  try { await p; } catch (e) { /* timeout is reported by the search caller */ }
}

function fail(e) {
  engineError = 'Engine error: ' + (e && e.message ? e.message : String(e));
  render();
}

async function engineTurn() {
  if (!ready || game.isGameOver() || game.turn() === human) return;
  const g = gen;
  render();
  let line;
  try {
    line = await searchBest(ucis.slice(), movetime);
  } catch (e) {
    if (g === gen) fail(e);
    return;
  }
  if (g !== gen) return;
  const best = line.split(/\s+/)[1];
  if (!best || best === '(none)' || best === '0000') { render(); return; }
  try {
    const m = game.move({ from: best.slice(0, 2), to: best.slice(2, 4), promotion: best[4] });
    ucis.push(best);
    lastMove = { from: m.from, to: m.to };
  } catch (e) {
    fail(new Error('illegal engine move ' + best));
    return;
  }
  render();
}

async function resetGame() {
  const g = ++gen;
  await stopSearch();
  if (g !== gen) return;
  human = $('side').value === 'white' ? 'w' : 'b';
  movetime = Number($('time').value);
  game = new Chess();
  ucis = [];
  selected = null;
  lastMove = null;
  $('info').textContent = '';
  if (ready) {
    try {
      send('setoption name LoseOpponent value ' + $('mode').value);
      send('ucinewgame');
      await sync();
    } catch (e) {
      fail(e);
      return;
    }
    if (g !== gen) return;
  }
  render();
  engineTurn();
}

async function undoMove() {
  const g = ++gen;
  await stopSearch();
  if (g !== gen) return;
  if (game.history().length) {
    do {
      game.undo();
      ucis.pop();
    } while (game.history().length && game.turn() !== human);
  }
  selected = null;
  const h = game.history({ verbose: true });
  lastMove = h.length ? { from: h[h.length - 1].from, to: h[h.length - 1].to } : null;
  render();
  engineTurn();
}

function humanMove(from, to, promotion) {
  try {
    game.move({ from, to, promotion });
  } catch (e) {
    return;
  }
  ucis.push(from + to + (promotion || ''));
  lastMove = { from, to };
  selected = null;
  render();
  engineTurn();
}

function askPromotion(from, to) {
  const box = $('promo').firstElementChild;
  box.textContent = '';
  for (const t of ['q', 'r', 'b', 'n']) {
    const b = document.createElement('button');
    b.type = 'button';
    const img = document.createElement('span');
    img.className = pieceClass(human, t);
    b.appendChild(img);
    b.addEventListener('click', (ev) => {
      ev.stopPropagation();
      $('promo').hidden = true;
      humanMove(from, to, t);
    });
    box.appendChild(b);
  }
  $('promo').hidden = false;
}

function onSquare(sq) {
  if (!ready || game.isGameOver() || game.turn() !== human) return;
  if (selected) {
    const legal = game.moves({ square: selected, verbose: true }).filter((m) => m.to === sq);
    if (legal.length) {
      if (legal[0].promotion) { askPromotion(selected, sq); return; }
      humanMove(selected, sq);
      return;
    }
  }
  const piece = game.get(sq);
  selected = piece && piece.color === human && selected !== sq ? sq : null;
  render();
}

function statusText() {
  if (engineError) return engineError;
  if (!ready) return 'Loading engine...';
  if (game.isCheckmate()) {
    return game.turn() === human
      ? 'Checkmate. Loserfish mated you (it had no other legal move).'
      : 'Checkmate. Loserfish got what it wanted.';
  }
  if (game.isStalemate()) return 'Stalemate. Draw: Loserfish failed to get mated.';
  if (game.isThreefoldRepetition()) return 'Draw by threefold repetition.';
  if (game.isInsufficientMaterial()) return 'Draw by insufficient material.';
  if (game.isDrawByFiftyMoves()) return 'Draw by the fifty-move rule.';
  if (game.turn() !== human) return 'Loserfish is thinking...';
  return game.inCheck() ? 'Your move. You are in check.' : 'Your move.';
}

function kingSquare(color) {
  const found = game.findPiece({ type: 'k', color });
  return found.length ? found[0] : null;
}

function render() {
  const boardEl = $('board');
  boardEl.textContent = '';
  const grid = game.board();
  const dests = new Map();
  if (selected) for (const m of game.moves({ square: selected, verbose: true })) dests.set(m.to, m.isCapture());
  const checked = game.inCheck() ? kingSquare(game.turn()) : null;
  const flip = human === 'b';
  for (let r = 0; r < 8; r++) {
    for (let c = 0; c < 8; c++) {
      const row = flip ? 7 - r : r;
      const col = flip ? 7 - c : c;
      const sq = FILES[col] + (8 - row);
      const el = document.createElement('div');
      el.className = 'sq ' + ((row + col) % 2 === 0 ? 'l' : 'd');
      el.dataset.sq = sq;
      if (lastMove && (lastMove.from === sq || lastMove.to === sq)) el.classList.add('last');
      if (selected === sq) el.classList.add('sel');
      if (checked === sq) el.classList.add('chk');
      if (dests.has(sq)) el.classList.add('dest', ...(dests.get(sq) ? ['cap'] : []));
      const piece = grid[row][col];
      if (piece) {
        const s = document.createElement('span');
        s.className = pieceClass(piece.color, piece.type);
        el.appendChild(s);
      }
      if (c === 0) { const n = document.createElement('span'); n.className = 'co r'; n.textContent = String(8 - row); el.appendChild(n); }
      if (r === 7) { const n = document.createElement('span'); n.className = 'co f'; n.textContent = FILES[col]; el.appendChild(n); }
      el.addEventListener('click', () => onSquare(sq));
      boardEl.appendChild(el);
    }
  }
  $('status').textContent = statusText();
  const list = $('moves');
  list.textContent = '';
  const hist = game.history();
  for (let i = 0; i < hist.length; i += 2) {
    const li = document.createElement('li');
    for (const t of [String(i / 2 + 1) + '.', hist[i], hist[i + 1] || '']) {
      const s = document.createElement('span');
      s.textContent = t;
      li.appendChild(s);
    }
    list.appendChild(li);
  }
  list.scrollTop = list.scrollHeight;
}

async function main() {
  $('new').addEventListener('click', resetGame);
  $('undo').addEventListener('click', undoMove);
  $('side').addEventListener('change', resetGame);
  $('mode').addEventListener('change', resetGame);
  $('time').addEventListener('change', () => { movetime = Number($('time').value); });
  $('promo').addEventListener('click', () => { $('promo').hidden = true; });
  human = $('side').value === 'white' ? 'w' : 'b';
  movetime = Number($('time').value);
  render();
  if (!globalThis.crossOriginIsolated) {
    const b = $('banner');
    b.hidden = false;
    b.textContent = 'The engine needs cross-origin isolation (SharedArrayBuffer). The page reloads once to enable it. ' +
      'If this message stays, service workers are blocked in this browser (private mode?).';
    return;
  }
  try {
    await initEngine();
  } catch (e) {
    fail(e);
    return;
  }
  ready = true;
  try {
    send('setoption name LoseOpponent value ' + $('mode').value);
    await sync();
  } catch (e) {
    fail(e);
    return;
  }
  render();
  engineTurn();
}

main();
