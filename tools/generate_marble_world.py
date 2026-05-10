"""Generate a medieval-room scene via World Labs Marble API.

Reads the API key from the WORLD_LABS_API_KEY environment variable.
Submits a generation request, polls the operation until complete,
and downloads the assets we'd want for in-game compositing
(panorama image, thumbnail, mesh) into ./marble_room/.

Run:
  WORLD_LABS_API_KEY=... python3 tools/generate_marble_world.py

Marble's text-to-world pipeline takes ~5 minutes per generation,
so the script polls every 30 s and reports progress to stdout.

The prompt is tuned for our use case: a medieval chamber with a
wooden chess table at the centre with the table TOP EMPTY (no
pieces / no board), since we want to render our own chessboard
+ clock model on top of it. Ambient warm candlelight so the
PBR shading on our pieces reads naturally.
"""

import json
import os
import sys
import time
import urllib.request
import urllib.error
from urllib.parse import urlparse


API_BASE = "https://api.worldlabs.ai"
GENERATE_URL = f"{API_BASE}/marble/v1/worlds:generate"
OUT_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "marble_room")

PROMPT = (
    "An immersive medieval European castle chamber. Heavy oak walls "
    "lined with stone arches, woven tapestries, and tall iron-banded "
    "windows letting in soft golden afternoon light. Wrought-iron "
    "candelabra against the side walls. Worn flagstone floor. The "
    "centre of the room is COMPLETELY EMPTY — no table, no chess "
    "board, no chess set, no furniture in the middle. The viewer is "
    "looking at the bare empty floor at the centre. Warm fireplace "
    "glow from one wall. Realistic, painterly, soft shadows, no "
    "people. Wide-angle interior view as if standing at the doorway "
    "looking toward the empty centre of the room."
)

DISPLAY_NAME = "3D-Chess medieval room"
MODEL = "marble-1.1"


def http_json(method, url, *, headers=None, body=None, timeout=60):
    headers = dict(headers or {})
    if body is not None:
        if "Content-Type" not in headers:
            headers["Content-Type"] = "application/json"
        data = json.dumps(body).encode()
    else:
        data = None
    req = urllib.request.Request(url, method=method, headers=headers, data=data)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read()
            return r.status, json.loads(raw) if raw else {}
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def http_download(url, out_path):
    print(f"  downloading {os.path.basename(out_path)}")
    with urllib.request.urlopen(url, timeout=300) as r, \
         open(out_path, "wb") as f:
        while True:
            chunk = r.read(64 * 1024)
            if not chunk:
                break
            f.write(chunk)


def main():
    api_key = os.environ.get("WORLD_LABS_API_KEY")
    if not api_key:
        sys.exit("error: WORLD_LABS_API_KEY env var not set")

    os.makedirs(OUT_DIR, exist_ok=True)
    headers = {"WLT-Api-Key": api_key}

    print(f"submitting generation: {DISPLAY_NAME!r}")
    print(f"  prompt: {PROMPT[:90]}...")
    status, body = http_json("POST", GENERATE_URL, headers=headers, body={
        "display_name": DISPLAY_NAME,
        "model":        MODEL,
        "world_prompt": {
            "type":        "text",
            "text_prompt": PROMPT,
        },
    })
    if status >= 300:
        sys.exit(f"submit failed ({status}): {body}")

    op_id  = body.get("operation_id") or body.get("name") or body.get("id")
    if not op_id:
        sys.exit(f"no operation id in response: {body}")
    print(f"operation id: {op_id}")

    op_url = f"{API_BASE}/marble/v1/operations/{op_id}"
    started = time.time()
    while True:
        time.sleep(30)
        status, op = http_json("GET", op_url, headers=headers)
        if status >= 300:
            print(f"  poll error ({status}): {op}")
            continue
        done    = op.get("done", False)
        state   = op.get("state") or op.get("status")
        elapsed = int(time.time() - started)
        print(f"  [{elapsed:4d}s] state={state} done={done}")
        if done:
            break

    if op.get("error"):
        sys.exit(f"generation failed: {op['error']}")

    # The completed op typically has a "response" / "world" field
    # with the world object.
    world = op.get("response") or op.get("world") or op
    world_id = world.get("world_id") or world.get("id")
    print(f"world id: {world_id}")
    print(f"viewer:   https://marble.worldlabs.ai/world/{world_id}")

    # Save the full operation response for later inspection.
    with open(os.path.join(OUT_DIR, "operation.json"), "w") as f:
        json.dump(op, f, indent=2)

    assets = world.get("assets", {})
    pano_url    = (assets.get("imagery") or {}).get("pano_url")
    thumb_url   = assets.get("thumbnail_url")
    mesh_url    = (assets.get("mesh") or {}).get("collider_mesh_url")
    splat_urls  = (assets.get("splats") or {}).get("spz_urls") or {}

    if pano_url:
        http_download(pano_url, os.path.join(OUT_DIR, "panorama.jpg"))
    if thumb_url:
        ext = os.path.splitext(urlparse(thumb_url).path)[1] or ".jpg"
        http_download(thumb_url, os.path.join(OUT_DIR, f"thumbnail{ext}"))
    if mesh_url:
        http_download(mesh_url, os.path.join(OUT_DIR, "collider.glb"))
    for tier in ("100k", "500k", "full_res"):
        u = splat_urls.get(tier)
        if u:
            http_download(u, os.path.join(OUT_DIR, f"splat_{tier}.spz"))

    print(f"\ndone. assets in {OUT_DIR}")


if __name__ == "__main__":
    main()
