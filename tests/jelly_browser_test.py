"""Headless WebGL smoke test. Requires Chrome, websockets and Pillow.
Run after make -C web; screenshots go to the directory passed as argv[1].
Uses the existing framebuffer-dump debug path, not compositor screenshots.
"""
import asyncio, base64, io, json, pathlib, subprocess, sys, tempfile, urllib.request
from PIL import Image
import websockets

ROOT=pathlib.Path(__file__).resolve().parents[1]
OUT=pathlib.Path(sys.argv[1] if len(sys.argv)>1 else '/tmp/chess-jelly-review')
OUT.mkdir(parents=True,exist_ok=True)

async def main():
    server=subprocess.Popen([sys.executable,'-m','http.server','8110','--bind','127.0.0.1'],cwd=ROOT/'web',stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    with tempfile.TemporaryDirectory(prefix='chess-jelly-browser-') as profile:
        chrome=subprocess.Popen(['google-chrome','--headless=new','--no-sandbox','--disable-dev-shm-usage','--enable-unsafe-swiftshader','--disable-background-timer-throttling','--disable-renderer-backgrounding','--remote-debugging-port=9241',f'--user-data-dir={profile}','--window-size=1280,960'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        try:
            for _ in range(80):
                try:
                    tabs=json.load(urllib.request.urlopen('http://127.0.0.1:9241/json'))
                    ws=next(t['webSocketDebuggerUrl'] for t in tabs if t.get('type')=='page'); break
                except Exception: await asyncio.sleep(.25)
            async with websockets.connect(ws,max_size=None) as sock:
                seq=0; errors=[]
                async def call(method,params=None):
                    nonlocal seq
                    seq+=1; await sock.send(json.dumps(dict(id=seq,method=method,params=params or {})))
                    while True:
                        msg=json.loads(await asyncio.wait_for(sock.recv(),90))
                        if msg.get('method')=='Runtime.exceptionThrown': errors.append(msg)
                        if msg.get('method')=='Runtime.consoleAPICalled' and msg['params']['type']=='error':
                            print('BROWSER ERROR',msg['params'].get('args'),flush=True);errors.append(msg)
                        if msg.get('method')=='Log.entryAdded' and msg['params']['entry']['level'] in ('error','warning'):
                            print('BROWSER LOG',msg['params']['entry']['text'],flush=True)
                        if msg.get('id')==seq:
                            if 'error' in msg: raise RuntimeError(msg)
                            return msg.get('result',{})
                async def ev(expr):
                    r=await call('Runtime.evaluate',dict(expression=expr,returnByValue=True))
                    if 'exceptionDetails' in r: raise RuntimeError(r)
                    return r.get('result',{}).get('value')
                async def probe(n): return await ev(f'Module._chess_dbg_jelly_probe({n})')
                async def point(c,r,lift):
                    return [await ev(f'Module._chess_dbg_square({c},{r},{lift},{i})') for i in (0,1)]
                async def mouse(kind,x,y,held=False):
                    box=await ev("(()=>{let r=document.querySelector('canvas').getBoundingClientRect();return {x:r.x,y:r.y}})()")
                    await call('Input.dispatchMouseEvent',dict(type=kind,x=x+box['x'],y=y+box['y'],button='left' if kind!='mouseMoved' else 'none',buttons=1 if held or kind=='mousePressed' else 0,clickCount=1 if kind!='mouseMoved' else 0))
                    await asyncio.sleep(.2)
                async def click(p):
                    await mouse('mouseMoved',*p)
                    await mouse('mousePressed',*p); await mouse('mouseReleased',*p)
                async def capture(name):
                    await ev('Module._chess_dbg_dump()')
                    b=await ev("(()=>{let d=Module.FS.readFile('/dump.ppm'),s='';for(let i=0;i<d.length;i+=8192)s+=String.fromCharCode(...d.subarray(i,i+8192));return btoa(s)})()")
                    im=Image.open(io.BytesIO(base64.b64decode(b))); im.save(OUT/(name+'.png'))
                    assert max(im.convert('RGB').getextrema()[0])>30, 'Blank render'
                    if name in ('jelly-board','jelly-stretched','jelly-menu'):
                        center=im.convert('RGB').crop((0,im.height//5,im.width,im.height*4//5))
                        assert sum(max(p)>35 for p in center.getdata())>center.width*center.height*.03, '3D scene missing (UI-only frame)'
                await call('Runtime.enable')
                await call('Log.enable')
                await call('Emulation.setDeviceMetricsOverride',dict(width=960,height=720,deviceScaleFactor=1,mobile=False))
                await call('Page.navigate',dict(url='http://127.0.0.1:8110/chess.html'))
                for _ in range(120):
                    if await ev("typeof Module!=='undefined' && !!Module._chess_dbg_jelly && Module.calledRun"): break
                    await asyncio.sleep(1)
                await asyncio.sleep(20)
                await ev('Module._chess_dbg_jelly(2,0)'); await asyncio.sleep(.5)
                await capture('jelly-options-before')
                size=await ev("(()=>{let c=document.querySelector('canvas');return [c.width,c.height]})()")
                await click([size[0]*.5,size[1]*.935])
                assert await probe(7)==1, 'Options toggle did not enable jelly'
                assert await ev("(localStorage.getItem('3d_chess_settings')||'').includes('jelly_pieces=1')"), 'Jelly option was not persisted'
                await capture('jelly-options')
                print('PASS options toggle',flush=True)
                await ev('Module._chess_dbg_jelly(1,1)'); await asyncio.sleep(3)
                await capture('jelly-board-before-selection')
                p=await point(3,1,.20)
                print('DIAG',size,'yaw',await probe(6),'points',p,await point(0,0,0),await point(7,7,0),flush=True)
                await mouse('mouseMoved',*p)
                await click(p)
                assert (await probe(1),await probe(2))==(3,1), f'Pawn selection failed: mode={await probe(0)} selected={(await probe(1),await probe(2))} point={p}'
                await capture('jelly-board')
                assert await probe(10)==0, 'WebGL error in refraction rendering'
                await ev('Module._chess_dbg_jelly_optics(1)')
                await capture('jelly-no-refraction')
                await ev('Module._chess_dbg_jelly_optics(1.36)')
                await capture('jelly-refraction')
                a=Image.open(OUT/'jelly-no-refraction.png').convert('RGB')
                b=Image.open(OUT/'jelly-refraction.png').convert('RGB')
                changed=sum(max(abs(x-y) for x,y in zip(p,q))>12 for p,q in zip(a.getdata(),b.getdata()))
                assert changed>300, f'IOR change did not affect refracted scene: {changed} pixels'
                print('PASS rendered scene, zero GL errors, refraction changes',changed,'pixels',flush=True)
                yaw=await probe(6)
                await mouse('mousePressed',*p)
                q=[p[0]+125,p[1]-85]
                await mouse('mouseMoved',*q,held=True); await asyncio.sleep(.6)
                assert await probe(5)==1, 'Drag did not grab piece'
                assert await probe(4)>.1, 'Piece did not stretch'
                assert await probe(11)>.17, 'Inverted volume element'
                assert await probe(12)<.06, 'Excessive volume drift'
                assert await probe(3)==0, 'Stretch changed board position'
                assert await probe(6)==yaw, 'Stretch orbited camera'
                await capture('jelly-stretched')
                await mouse('mouseReleased',*q)
                assert await probe(5)==0
                assert (await probe(1),await probe(2))==(3,1), 'Release lost selection'
                print('PASS selection, stretch, unchanged board, camera isolation, release',flush=True)
                for _ in range(30):
                    if await probe(4)<.02: break
                    await asyncio.sleep(1)
                assert await probe(4)<.02, f'Jelly failed to settle: {await probe(4)}'
                await click(await point(3,3,.01))
                assert await probe(3)==1, 'Destination click did not make exactly one move'
                await capture('jelly-moved')
                print('PASS selection, stretch, camera isolation, release, settle, e2-e4',flush=True)
                await ev('Module._chess_dbg_jelly(0,1)'); await asyncio.sleep(3)
                wobble=max([await probe(9) for _ in range(3)])
                assert wobble>0, 'Menu collisions did not excite jelly'
                await capture('jelly-menu')
                print('PASS menu collision wobble',flush=True)
                assert not errors, errors
                print('PASS no browser runtime exceptions; screenshots:',OUT,flush=True)
        finally:
            chrome.terminate(); server.terminate()
            chrome.wait(timeout=15); server.wait(timeout=15)

asyncio.run(main())
