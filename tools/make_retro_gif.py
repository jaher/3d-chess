#!/usr/bin/env python3
"""Render an animated GIF of a retro piece's selection animation, headless.

Drives the web build (served on :8099) via CDP: enters the cable-room retro
game, then for each frame forces the piece type's animated part to a phase
(chess_dbg_animate), dumps the framebuffer (chess_dbg_dump -> /dump.ppm), pulls
it out of MEMFS, crops to the piece, and assembles a looping GIF with PIL.

Usage: python3 tools/make_retro_gif.py <type_index> <out.gif> [cycle_s] [frames] [crop]
  type_index: KING=0 QUEEN=1 BISHOP=2 KNIGHT=3 ROOK=4 PAWN=5
  crop: "x0,y0,x1,y1" as fractions of the frame (default a back-corner piece)
Requires the web build already built + served: python3 -m http.server 8099 in web/.
"""
import asyncio, json, subprocess, time, urllib.request, base64, sys, io, os
import websockets
from PIL import Image

TYPE = int(sys.argv[1]) if len(sys.argv) > 1 else 4
OUT = sys.argv[2] if len(sys.argv) > 2 else "/tmp/retro.gif"
CYCLE = float(sys.argv[3]) if len(sys.argv) > 3 else 0.667   # seconds for one loop
FRAMES = int(sys.argv[4]) if len(sys.argv) > 4 else 24
CROP = sys.argv[5] if len(sys.argv) > 5 else "0.55,0.20,0.95,0.62"
cx0, cy0, cx1, cy1 = [float(x) for x in CROP.split(",")]
PORT = int(os.environ.get("CDP_PORT","9223"))
HTTP = os.environ.get("HTTP_PORT","8099")

subprocess.run(f"ps -eo pid,args | awk '/remote-debugging-port={PORT}/ && !/awk/{{print $1}}' | xargs -r kill", shell=True); time.sleep(1)
subprocess.Popen(["google-chrome","--headless=new","--no-sandbox","--disable-dev-shm-usage","--enable-unsafe-swiftshader",
                  f"--remote-debugging-port={PORT}","--window-size=1600,1200"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
ws=None
for _ in range(15):
    try:
        tabs=json.load(urllib.request.urlopen(f"http://localhost:{PORT}/json"))
        p=[t for t in tabs if t.get("type")=="page"]
        if p: ws=p[0]["webSocketDebuggerUrl"]; break
    except Exception: pass
    time.sleep(1)

async def main():
    frames=[]
    async with websockets.connect(ws, max_size=None) as w:
        n=[0]; pend={}
        async def call(m,p=None):
            n[0]+=1; mid=n[0]; await w.send(json.dumps({"id":mid,"method":m,"params":p or {}})); return mid
        async def wait(mid,sec=8):
            end=time.time()+sec
            while time.time()<end:
                try: m=json.loads(await asyncio.wait_for(w.recv(),timeout=end-time.time()))
                except asyncio.TimeoutError: break
                if m.get("id")==mid: return m.get("result")
            return None
        async def ev(expr):
            mid=await call("Runtime.evaluate",{"expression":expr,"returnByValue":True}); r=await wait(mid)
            return (r or {}).get("result",{}).get("value")
        await call("Page.enable"); await call("Runtime.enable")
        await call("Network.enable"); await call("Network.setCacheDisabled",{"cacheDisabled":True})
        mid=await call("Page.navigate",{"url":f"http://localhost:{HTTP}/chess.html"}); await wait(mid,3)
        await asyncio.sleep(14)
        await ev("Module.ccall('chess_dbg_cable_game',null,[],[])"); await asyncio.sleep(1.0)
        for i in range(FRAMES):
            t = CYCLE * i / FRAMES
            await ev(f"Module.ccall('chess_dbg_animate',null,['number','number'],[{TYPE},{t}])")
            await ev("Module.ccall('chess_dbg_dump',null,[],[])")
            b64 = await ev("(function(){try{var d=Module.FS.readFile('/dump.ppm');var s='';for(var i=0;i<d.length;i++)s+=String.fromCharCode(d[i]);return btoa(s);}catch(e){return 'ERR'+e;}})()")
            if not b64 or b64.startswith("ERR"): print("dump fail", b64); continue
            im = Image.open(io.BytesIO(base64.b64decode(b64)))
            W,H = im.size
            crop = im.crop((int(W*cx0),int(H*cy0),int(W*cx1),int(H*cy1)))
            crop = crop.resize((crop.width*2, crop.height*2), Image.LANCZOS)
            frames.append(crop)
            print(f"frame {i+1}/{FRAMES} t={t:.3f}")
    if frames:
        frames[0].save(OUT, save_all=True, append_images=frames[1:], duration=int(CYCLE*1000/len(frames)), loop=0, disposal=2)
        print("wrote", OUT, len(frames), "frames", frames[0].size)
asyncio.run(main())
