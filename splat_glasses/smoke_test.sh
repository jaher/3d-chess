#!/usr/bin/env bash
# splat_glasses smoke test — no network, no API key needed.
#
# Verifies the whole local pipeline: renderer builds, loads a real SPZ,
# answers pose requests with valid JPEGs; the Node server boots, streams
# frames over HTTP (/frame.jpg + /stream.mjpg) and applies nav input.
#
# Usage: bash smoke_test.sh   (or: make test)
set -euo pipefail
cd "$(dirname "$0")"

SPZ=../world_labs/datacenter/splat_500k.spz
PORT=18095
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"; [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2>/dev/null || true' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -f "$SPZ" ] || fail "test asset missing: $SPZ"
make -s splat_render

# --- 1. renderer alone: two poses -> two JPEGs -----------------------------
printf 'P 0 0 0 0 0\nP 0 0 0 1.5708 0\nQ\n' \
  | ./splat_render "$SPZ" 320 320 > "$TMP/frames.bin" 2> "$TMP/render.log"
python3 - "$TMP" <<'EOF'
import sys, os
tmp = sys.argv[1]
d = open(os.path.join(tmp, 'frames.bin'), 'rb').read()
frames, i = [], 0
while i < len(d):
    nl = d.index(b'\n', i)
    assert d[i:i+2] == b'F ', 'bad frame header'
    n = int(d[i+2:nl])
    frames.append(d[nl+1:nl+1+n])
    i = nl + 1 + n
assert len(frames) == 2, f'expected 2 frames, got {len(frames)}'
for f in frames:
    assert f[:2] == b'\xff\xd8' and f[-2:] == b'\xff\xd9', 'not a complete JPEG'
    assert len(f) > 5000, 'suspiciously small frame (blank render?)'
assert frames[0] != frames[1], 'different poses rendered identical frames'
print(f'renderer ok: {len(frames)} JPEGs, {len(frames[0])} / {len(frames[1])} bytes')
EOF

# --- 2. server: boot, one-shot frame, nav, mjpeg ---------------------------
node server.js "$PORT" > "$TMP/server.log" 2>&1 &
SRV_PID=$!
for i in $(seq 1 40); do
  curl -sf "http://localhost:$PORT/frame.jpg" -o "$TMP/f1.jpg" 2>/dev/null && break
  sleep 0.5
  kill -0 "$SRV_PID" 2>/dev/null || { cat "$TMP/server.log"; fail "server died"; }
done
[ -s "$TMP/f1.jpg" ] || { cat "$TMP/server.log"; fail "no frame from server"; }

curl -sf -X POST "http://localhost:$PORT/api/nav" -d '{"action":"left"}' > /dev/null
sleep 1
curl -sf "http://localhost:$PORT/frame.jpg" -o "$TMP/f2.jpg"
cmp -s "$TMP/f1.jpg" "$TMP/f2.jpg" && fail "nav did not change the frame"

curl -sf --max-time 4 "http://localhost:$PORT/stream.mjpg" -o "$TMP/mjpeg.bin" || true
grep -q "Content-Type: image/jpeg" "$TMP/mjpeg.bin" || fail "mjpeg stream has no jpeg part"

curl -sf "http://localhost:$PORT/api/status" | grep -q '"rendererUp":true' || fail "status says renderer down"

echo "smoke test ok: renderer + server + nav + mjpeg all good"
