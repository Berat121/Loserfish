// Smoke test for the Node WASM build: node tests/wasm_smoke.cjs build-wasm/loserfish.js
// Expects the same node count as the native engine (single thread, deterministic).
const path = require('path');
const Loserfish = require(path.resolve(process.argv[2] || 'build-wasm/loserfish.js'));
const lines = [];
const wait = (ms) => new Promise((r) => setTimeout(r, ms));
(async () => {
  const M = await Loserfish({ print: (l) => lines.push(String(l)), printErr: () => {}, arguments: [] });
  const cmd = (s) => M.ccall('loserfish_cmd', null, ['string'], [s]);
  const until = async (re, ms) => { for (let t = 0; t < ms; t += 50) { if (lines.some((l) => re.test(l))) return true; await wait(50); } return false; };
  cmd('uci');
  const ok1 = await until(/^uciok/, 5000);
  cmd('position startpos moves e2e4 e7e5');
  cmd('go depth 8');
  const ok2 = await until(/^bestmove/, 10000);
  const d8 = lines.find((l) => /^info depth 8 /.test(l)) || '';
  const ok3 = /nodes 612803 /.test(d8);
  console.log(ok1 ? 'ok   uciok' : 'FAIL uciok');
  console.log(ok2 ? 'ok   bestmove' : 'FAIL bestmove');
  console.log(ok3 ? 'ok   depth 8 nodes 612803 (same as native)' : 'FAIL depth 8 nodes: ' + d8);
  cmd('quit'); await wait(300);
  const good = ok1 && ok2 && ok3;
  console.log(good ? 'ALL OK' : 'FAILED');
  process.exit(good ? 0 : 1);
})();
