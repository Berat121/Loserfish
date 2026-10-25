import { Chess } from './vendor/chess.js';

const $ = (id) => document.getElementById(id);
const FILES = 'abcdefgh';
const SETUP_TIMEOUT_MS = 30000;
const DEPTH_TIMEOUT_MS = 10 * 60 * 1000;
const raf = (f) => globalThis.requestAnimationFrame(f);

for (const c of 'wb') {
  for (const t of 'KQRBNP') {
    const img = document.createElement('img');
    img.src = 'pieces/cburnett/' + c + t + '.svg';
  }
}

const keyOf = (color, type) => color + type.toUpperCase();

let M = null;
let waiter = null;
let searchPromise = null;
let game = new Chess();
let ucis = [];
let human = 'w';
let flip = false;
let limit = { kind: 'time', value: 1000 };
let gen = 0;
let selected = null;
let lastMove = null;
let ready = false;
let engineError = null;
let engineScoreMode = 'neutral';
let goal = null;
let drag = null;
const pieceEls = new Map();

/* ---------- engine ---------- */

function onLine(raw) {
  const line = String(raw);
  if (line.startsWith('info ') && line.includes(' depth ')) {
    const d = /\bdepth (\d+)/.exec(line);
    const n = /\bnps (\d+)/.exec(line);
    const s = /\bscore (cp|mate) (-?\d+)/.exec(line);
    if (s && engineScoreMode !== 'neutral') {
      const v = Number(s[2]);
      goal = { kind: s[1], v: engineScoreMode === 'natural' ? -v : v };
    }
    if (d) {
      $('info').textContent = 'depth ' + d[1] + (n ? ' \u00b7 ' + (Number(n[1]) / 1e6).toFixed(2) + ' Mnps' : '');
      renderEval();
    }
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

function setScoreOption() {
  const v = $('eval').value;
  engineScoreMode = v === 'off' ? 'neutral' : v;
  send('setoption name LoseScore value ' + engineScoreMode);
}

function searchBest(list, lim) {
  const p = expect(/^bestmove /, lim.kind === 'time' ? lim.value + 20000 : DEPTH_TIMEOUT_MS);
  searchPromise = p;
  const clear = () => { if (searchPromise === p) searchPromise = null; };
  p.then(clear, clear);
  send(list.length ? 'position startpos moves ' + list.join(' ') : 'position startpos');
  send(lim.kind === 'time' ? 'go movetime ' + lim.value : 'go depth ' + lim.value);
  return p;
}

async function stopSearch() {
  const p = searchPromise;
  if (!p) return;
  send('stop');
  try { await p; } catch (e) { /* the search caller reports timeouts */ }
}

function fail(e) {
  engineError = 'Engine error: ' + (e && e.message ? e.message : String(e));
  render();
}

/* ---------- board geometry and pieces ---------- */

function posOf(sq) {
  let c = FILES.indexOf(sq[0]);
  let r = 8 - Number(sq[1]);
  if (flip) { c = 7 - c; r = 7 - r; }
  return [c, r];
}

function place(el, sq) {
  const [c, r] = posOf(sq);
  el.style.transform = 'translate(' + c * 100 + '%, ' + r * 100 + '%)';
  el.dataset.sq = sq;
}

function squareAt(x, y) {
  const r = $('board').getBoundingClientRect();
  if (!r.width) return null;
  let col = Math.floor(((x - r.left) / r.width) * 8);
  let row = Math.floor(((y - r.top) / r.height) * 8);
  if (col < 0 || col > 7 || row < 0 || row > 7) return null;
  if (flip) { col = 7 - col; row = 7 - row; }
  return FILES[col] + (8 - row);
}

const dist = (a, b) => Math.max(Math.abs(FILES.indexOf(a[0]) - FILES.indexOf(b[0])), Math.abs(Number(a[1]) - Number(b[1])));

function syncPieces() {
  const layer = $('pieces');
  const want = new Map();
  game.board().forEach((row, r) => row.forEach((p, c) => {
    if (p) want.set(FILES[c] + (8 - r), keyOf(p.color, p.type));
  }));
  const free = [];
  for (const [sq, el] of [...pieceEls]) {
    if (want.get(sq) === el.dataset.k) want.delete(sq);
    else { pieceEls.delete(sq); free.push(el); }
  }
  const take = (k, sq) => {
    let best = -1;
    let bestD = 99;
    free.forEach((el, i) => {
      if (el.dataset.k !== k) return;
      const d = lastMove && lastMove.to === sq && el.dataset.sq === lastMove.from ? -1 : dist(el.dataset.sq, sq);
      if (d < bestD) { bestD = d; best = i; }
    });
    return best < 0 ? null : free.splice(best, 1)[0];
  };
  for (const [sq, k] of want) {
    let el = take(k, sq);
    if (!el) {
      el = document.createElement('div');
      el.className = 'p ' + k;
      el.dataset.k = k;
      layer.appendChild(el);
    }
    place(el, sq);
    pieceEls.set(sq, el);
  }
  for (const el of free) el.remove();
}

function rebuildPieces() {
  const board = $('board');
  board.classList.add('noanim');
  for (const el of pieceEls.values()) el.remove();
  pieceEls.clear();
  syncPieces();
  raf(() => raf(() => board.classList.remove('noanim')));
}

function renderCoords() {
  const box = $('coords');
  box.textContent = '';
  for (let i = 0; i < 8; i++) {
    const rank = document.createElement('span');
    const rr = flip ? 7 - i : i;
    rank.className = 'co ' + (i % 2 === 0 ? 'd' : 'l');
    rank.textContent = String(8 - rr);
    rank.style.left = '3px';
    rank.style.top = 'calc(' + i * 12.5 + '% + 3px)';
    box.appendChild(rank);
    const file = document.createElement('span');
    const cc = flip ? 7 - i : i;
    file.className = 'co ' + (i % 2 === 0 ? 'l' : 'd');
    file.textContent = FILES[cc];
    file.style.right = 'calc(' + (7 - i) * 12.5 + '% + 3px)';
    file.style.bottom = '2px';
    box.appendChild(file);
  }
}

function kingSquare(color) {
  const found = game.findPiece({ type: 'k', color });
  return found.length ? found[0] : null;
}

function renderHighlights() {
  const frag = document.createDocumentFragment();
  const add = (sq, cls) => {
    const el = document.createElement('div');
    el.className = 'hl ' + cls;
    place(el, sq);
    frag.appendChild(el);
  };
  if (lastMove) { add(lastMove.from, 'last'); add(lastMove.to, 'last'); }
  if (selected) add(selected, 'sel');
  if (game.inCheck()) {
    const k = kingSquare(game.turn());
    if (k) add(k, 'chk');
  }
  if (selected) for (const m of game.moves({ square: selected, verbose: true })) add(m.to, m.isCapture() ? 'dest cap' : 'dest');
  $('hl').replaceChildren(frag);
}

/* ---------- eval ---------- */

const EVAL_HINT = {
  inverted: 'Inverted: the sign of Loserfish\u2019s goal. Higher means closer to being mated, and its colour grows on the bar. #N: it gets mated in N moves.',
  natural: 'Natural: like a normal engine. The bar shows who stands better in the usual sense, so Loserfish looks worse as it nears being mated. #-N: it gets mated in N moves.',
  off: 'Eval is hidden.',
};

const MODE_HINT = {
  adversarial: 'Adversarial: Loserfish assumes you play the worst moves for its goal, so it never relies on you to mate it. Default and strongest.',
  cooperative: 'Cooperative: Loserfish assumes you mate it whenever you can. Fast against opponents who play for a win, stalls against a refusing one.',
};

function evalValue(mode) {
  if (!goal) return null;
  const sign = mode === 'natural' ? -1 : 1;
  return { kind: goal.kind, v: sign * goal.v };
}

function renderEval() {
  const mode = $('eval').value;
  const bar = $('evalbar');
  const fill = $('evalfill');
  const text = $('evaltext');
  bar.classList.toggle('off', mode === 'off' || !goal);
  bar.classList.toggle('flip', flip);
  const ev = mode === 'off' ? null : evalValue(mode);
  let whiteShare = 0.5;
  if (ev) {
    const cp = ev.kind === 'mate' ? (ev.v < 0 ? -2000 : 2000) : Math.max(-1000, Math.min(1000, ev.v));
    const win = 1 / (1 + Math.exp(-0.00368208 * cp));
    const engineIsWhite = human === 'b';
    whiteShare = engineIsWhite ? win : 1 - win;
    text.textContent = ev.kind === 'mate' ? '#' + ev.v : (ev.v > 0 ? '+' : '') + (ev.v / 100).toFixed(1);
  } else {
    text.textContent = '';
  }
  const bottomShare = flip ? 1 - whiteShare : whiteShare;
  fill.style.transform = 'scaleY(' + bottomShare.toFixed(4) + ')';
  text.style.color = bottomShare >= 0.5 ? (flip ? '#f2f2f2' : '#2b2b2b') : (flip ? '#2b2b2b' : '#f2f2f2');
  text.style.bottom = bottomShare >= 0.5 ? '3px' : 'auto';
  text.style.top = bottomShare >= 0.5 ? 'auto' : '3px';
  $('evalhint').textContent = EVAL_HINT[mode];
}

/* ---------- game flow ---------- */

async function engineTurn() {
  if (!ready || game.isGameOver() || game.turn() === human) return;
  const g = gen;
  render();
  let line;
  try {
    line = await searchBest(ucis.slice(), limit);
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
  game = new Chess();
  ucis = [];
  selected = null;
  lastMove = null;
  goal = null;
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
  const newFlip = human === 'b';
  if (newFlip !== flip) { flip = newFlip; renderCoords(); }
  rebuildPieces();
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
  goal = null;
  const h = game.history({ verbose: true });
  lastMove = h.length ? { from: h[h.length - 1].from, to: h[h.length - 1].to } : null;
  render();
  engineTurn();
}

async function applyScoreMode() {
  if (ready) {
    if (searchPromise) { try { await searchPromise; } catch (e) { /* reported by the search */ } }
    setScoreOption();
    if (engineScoreMode === 'neutral') goal = null;
  }
  renderEval();
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
    img.className = 'pc ' + keyOf(human, t);
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

function tryMoveTo(from, to) {
  const legal = game.moves({ square: from, verbose: true }).filter((m) => m.to === to);
  if (!legal.length) return false;
  if (legal[0].promotion) {
    selected = from;
    askPromotion(from, to);
    return true;
  }
  humanMove(from, to);
  return true;
}

/* ---------- pointer input (drag, ghost, click-click) ---------- */

function releaseDrag(d, instant) {
  if (d.raf) globalThis.cancelAnimationFrame(d.raf);
  if (d.ghost) d.ghost.remove();
  if (d.el) {
    d.el.classList.remove('dragging');
    if (instant) {
      d.el.style.transition = 'none';
      raf(() => { d.el.style.transition = ''; });
    }
  }
  document.body.style.cursor = '';
}

function onPointerDown(e) {
  if (e.button > 0 || drag) return;
  if (!ready || game.isGameOver() || game.turn() !== human) return;
  const sq = squareAt(e.clientX, e.clientY);
  if (!sq) return;
  e.preventDefault();
  if (selected && tryMoveTo(selected, sq)) return;
  const piece = game.get(sq);
  if (piece && piece.color === human) {
    const wasSelected = selected === sq;
    selected = sq;
    renderHighlights();
    drag = {
      id: e.pointerId, from: sq, piece, el: pieceEls.get(sq), x0: e.clientX, y0: e.clientY, x: e.clientX, y: e.clientY,
      moved: false, wasSelected, gen, ghost: null, size: 0, rect: null, over: null, raf: 0,
      dests: new Set(game.moves({ square: sq, verbose: true }).map((m) => m.to)),
    };
    try { $('board').setPointerCapture(e.pointerId); } catch (err) { /* document listeners still receive events */ }
  } else {
    selected = null;
    renderHighlights();
  }
}

function applyDrag() {
  if (!drag) return;
  drag.raf = 0;
  const r = drag.rect;
  drag.el.style.transform = 'translate(' + (drag.x - r.left - drag.size / 2) + 'px, ' + (drag.y - r.top - drag.size / 2) + 'px)';
}

function onPointerMove(e) {
  if (!drag || e.pointerId !== drag.id) return;
  if (drag.gen !== gen) { releaseDrag(drag, false); drag = null; render(); return; }
  drag.x = e.clientX;
  drag.y = e.clientY;
  if (!drag.moved) {
    if (Math.hypot(e.clientX - drag.x0, e.clientY - drag.y0) < 4 || !drag.el) return;
    drag.moved = true;
    drag.rect = $('board').getBoundingClientRect();
    drag.size = drag.rect.width / 8;
    const ghost = document.createElement('div');
    ghost.className = 'p ghost ' + drag.el.dataset.k;
    place(ghost, drag.from);
    $('pieces').insertBefore(ghost, drag.el);
    drag.ghost = ghost;
    drag.el.classList.add('dragging');
    document.body.style.cursor = 'grabbing';
  }
  if (!drag.raf) drag.raf = raf(applyDrag);
  const sq = squareAt(e.clientX, e.clientY);
  if (sq !== drag.over) {
    const prev = drag.over && document.querySelector('#hl .dest[data-sq="' + drag.over + '"]');
    if (prev) prev.classList.remove('over');
    drag.over = sq;
    const cur = sq && drag.dests.has(sq) && document.querySelector('#hl .dest[data-sq="' + sq + '"]');
    if (cur) cur.classList.add('over');
  }
}

function onPointerUp(e) {
  if (!drag || e.pointerId !== drag.id) return;
  const d = drag;
  drag = null;
  if (d.gen !== gen) { releaseDrag(d, false); render(); return; }
  if (!d.moved) {
    releaseDrag(d, false);
    if (d.wasSelected) { selected = null; renderHighlights(); }
    return;
  }
  const sq = squareAt(e.clientX, e.clientY);
  const legal = sq && d.dests.has(sq);
  const promo = legal && game.moves({ square: d.from, verbose: true }).some((m) => m.to === sq && m.promotion);
  releaseDrag(d, legal && !promo);
  if (legal && tryMoveTo(d.from, sq)) {
    if (promo) place(d.el, d.from);
    return;
  }
  place(d.el, d.from);
  renderHighlights();
}

function onPointerCancel(e) {
  if (!drag || e.pointerId !== drag.id) return;
  const d = drag;
  drag = null;
  releaseDrag(d, false);
  if (d.el) place(d.el, d.from);
  renderHighlights();
}

/* ---------- PGN ---------- */

function buildPgn() {
  const pad = (n) => String(n).padStart(2, '0');
  const now = new Date();
  let result = '*';
  if (game.isCheckmate()) result = game.turn() === 'w' ? '0-1' : '1-0';
  else if (game.isDraw()) result = '1/2-1/2';
  game.setHeader('Event', 'Loserfish demo');
  game.setHeader('Site', 'https://berat121.github.io/Loserfish/');
  game.setHeader('Date', now.getFullYear() + '.' + pad(now.getMonth() + 1) + '.' + pad(now.getDate()));
  game.setHeader('White', human === 'w' ? 'You' : 'Loserfish');
  game.setHeader('Black', human === 'w' ? 'Loserfish' : 'You');
  game.setHeader('Result', result);
  return game.pgn();
}

function toast(msg) {
  $('toast').textContent = msg;
  clearTimeout(toast.t);
  toast.t = setTimeout(() => { $('toast').textContent = ''; }, 2500);
}

function exportPgn() {
  if (!game.history().length) { toast('No moves yet.'); return; }
  const blob = new Blob([buildPgn() + '\n'], { type: 'application/x-chess-pgn' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  const d = new Date();
  a.href = url;
  a.download = 'loserfish-' + d.toISOString().slice(0, 16).replace(/[-:T]/g, '') + '.pgn';
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
  toast('PGN downloaded.');
}

async function copyPgn() {
  if (!game.history().length) { toast('No moves yet.'); return; }
  try {
    await navigator.clipboard.writeText(buildPgn());
    toast('PGN copied.');
  } catch (e) {
    toast('Copy failed. Use Export PGN.');
  }
}

/* ---------- rendering ---------- */

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

function render() {
  syncPieces();
  renderHighlights();
  const thinking = ready && !game.isGameOver() && game.turn() !== human && !engineError;
  const over = game.isGameOver();
  $('status').textContent = statusText();
  $('dot').className = 'dot' + (thinking ? ' think' : over ? ' over' : '');
  $('now').hidden = !thinking;
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
  renderEval();
}

function readLimit() {
  const v = $('limit').value;
  limit = { kind: v[0] === 't' ? 'time' : 'depth', value: Number(v.slice(1)) };
}

async function main() {
  $('new').addEventListener('click', resetGame);
  $('undo').addEventListener('click', undoMove);
  $('side').addEventListener('change', resetGame);
  $('mode').addEventListener('change', () => { $('modehint').textContent = MODE_HINT[$('mode').value]; resetGame(); });
  $('limit').addEventListener('change', readLimit);
  $('eval').addEventListener('change', applyScoreMode);
  $('now').addEventListener('click', () => { if (searchPromise) send('stop'); });
  $('pgn').addEventListener('click', exportPgn);
  $('copy').addEventListener('click', copyPgn);
  $('promo').addEventListener('click', () => { $('promo').hidden = true; });
  $('board').addEventListener('pointerdown', onPointerDown);
  document.addEventListener('pointermove', onPointerMove);
  document.addEventListener('pointerup', onPointerUp);
  document.addEventListener('pointercancel', onPointerCancel);
  human = $('side').value === 'white' ? 'w' : 'b';
  flip = human === 'b';
  readLimit();
  $('modehint').textContent = MODE_HINT[$('mode').value];
  renderCoords();
  rebuildPieces();
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
    setScoreOption();
    await sync();
  } catch (e) {
    fail(e);
    return;
  }
  render();
  engineTurn();
}

main();
