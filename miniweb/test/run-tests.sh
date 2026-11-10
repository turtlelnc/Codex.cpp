#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p test/out

BIN="./build/miniweb"
if [ ! -x "$BIN" ]; then
  ./build.sh
fi

run() {
  name="$1"; shift
  echo "== $name"
  "$BIN" shot "test/$name.html" -o "test/out/$name.png" --wait 2000 --full-page "$@" >/dev/null
}

run page --width 1000 --height 700
run js --width 900 --height 500
run react --width 900 --height 520
run vue --width 900 --height 520
run grid --width 960 --height 760
run gap --width 800 --height 260
run oklch --width 700 --height 320
run colormix --width 600 --height 220
run svg --width 820 --height 360
run svg-img --width 700 --height 220
run bg --width 800 --height 560
run aspect --width 700 --height 400
run module --width 700 --height 360
run modules --width 760 --height 300
run blocked --width 700 --height 420
run react-icons --width 820 --height 300
echo "== click (native dispatch)"
"$BIN" shot test/click.html -o test/out/click.png --width 600 --height 260 --full-page --click 100,90 >/dev/null

# Loopback HTTP tests (two small static servers on 127.0.0.1).
python3 -m http.server 8765 --bind 127.0.0.1 --directory test/assets >/tmp/miniweb-test-8765.log 2>&1 &
P1=$!
python3 -m http.server 8766 --bind 127.0.0.1 --directory test/assets >/tmp/miniweb-test-8766.log 2>&1 &
P2=$!
trap 'kill $P1 $P2 2>/dev/null || true' EXIT INT TERM
for i in $(seq 1 30); do
  curl -s -o /dev/null http://127.0.0.1:8765/gradient.png && break
  python3 -c 'import time; time.sleep(0.1)'
done
run loopback --width 800 --height 420
"$BIN" shot http://127.0.0.1:8765/loop.html -o test/out/loop-page.png --width 760 --height 380 --full-page >/dev/null
run react-fetch --width 900 --height 500

echo "all renders written to test/out/"
