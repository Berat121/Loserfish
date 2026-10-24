#!/bin/sh
# Build Loserfish for WebAssembly with Emscripten (emcc/em++ must be on PATH).
# Usage: scripts/build_wasm.sh [web|node] [outdir]
# Output: <outdir>/loserfish.js and loserfish.wasm (MODULARIZE factory named Loserfish).
# Commands are sent with Module.ccall('loserfish_cmd', null, ['string'], ['uci']); engine output
# arrives through the Module.print callback. The engine runs on a pthread (PROXY_TO_PTHREAD), so
# a browser needs cross-origin isolation (COOP/COEP) for SharedArrayBuffer.
set -e
TARGET=${1:-web}
OUT=${2:-build-wasm}
cd "$(dirname "$0")/../src"
mkdir -p "../$OUT/obj"
case "$TARGET" in
  web)  ENV='web,worker' ;;
  node) ENV='node' ;;
  *) echo "target must be web or node" >&2; exit 1 ;;
esac
pids=""
for f in *.cpp syzygy/*.cpp; do
  o="../$OUT/obj/$(echo "$f" | tr / _).o"
  em++ -O2 -std=c++17 -fno-exceptions -pthread -DNDEBUG -c "$f" -o "$o" &
  pids="$pids $!"
done
for p in $pids; do wait "$p"; done
em++ -O2 -pthread -fno-exceptions "../$OUT"/obj/*.o -o "../$OUT/loserfish.js" \
  -sENVIRONMENT="$ENV" -sMODULARIZE -sEXPORT_NAME=Loserfish \
  -sEXPORTED_FUNCTIONS=_main,_loserfish_cmd -sEXPORTED_RUNTIME_METHODS=ccall,cwrap \
  -sINITIAL_MEMORY=268435456 -sSTACK_SIZE=4194304 -sDEFAULT_PTHREAD_STACK_SIZE=4194304 \
  -sPTHREAD_POOL_SIZE=2 -sPROXY_TO_PTHREAD -sEXIT_RUNTIME=1
ls -l "../$OUT/loserfish.js" "../$OUT/loserfish.wasm"
