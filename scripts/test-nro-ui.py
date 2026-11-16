#!/usr/bin/env python3
"""Exercise the real XML/Borealis NRO UI with a local-only backend fixture.

Build tests/ui first. Requires a desktop display (or xvfb-run), SDL2 and Mesa.
Optional screenshots are OpenGL framebuffer PPMs, not HTML approximations.
"""
import argparse
import http.server
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import struct
import zlib
from urllib.parse import parse_qs,urlsplit

def thumbnail():
    def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    raw=b''.join(b'\x00'+bytes(value for x in range(64) for value in (50+x*3,35+y*2,155)) for y in range(64))
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',64,64,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b'')

class Backend(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_GET(self):
        if self.path.startswith('/v1/artwork/'):
            raw=thumbnail();self.send_response(200);self.send_header('Content-Type','image/png');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw);return
        if self.path=='/auth/me':payload={'user':{'username':'demo','apple_linked':True},'csrf':'synthetic-csrf'}
        elif self.path.startswith('/v1/lyrics/'):
            payload={'provider':'syncedlyrics','lines':[{'time_ms':0,'text':'A little closer to the music'},
                {'time_ms':10000,'text':'Let the night carry us home'},
                {'time_ms':20000,'text':'Every moment has a melody'}]}
        elif self.path.startswith('/v1/catalog/search?'):
            term=parse_qs(urlsplit(self.path).query).get('term',[''])[0]
            payload={'items':[{'id':str(100+i),'type':'albums' if term=='preview' or i%3==0 else 'songs',
                'title':('A very long album title that should remain readable beside its cover without overlapping the duration column' if term=='long' else 'Track '+str(i+1)),
                'artist':'Artist name','duration_ms':215000,'artwork_path':'/v1/artwork/songs/'+str(100+i)} for i in range(12)] if term!='empty' else [],'next_offset':12}
        elif self.path.startswith('/v1/catalog/albums/') and self.path.endswith('/tracks'):payload={'items':[]}
        else:self.send_error(404);return
        raw=json.dumps(payload).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--screenshots',type=Path)
    scenes=('home','login','guest','search','settings','qr','collection','long','empty','player','navigation')
    parser.add_argument('--scene',choices=scenes)
    args=parser.parse_args();binary=args.binary.resolve()
    output=args.screenshots.resolve() if args.screenshots else None
    if output:output.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='switchmusic-ui-') as directory,http.server.ThreadingHTTPServer(('127.0.0.1',0),Backend) as server:
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        try:
            environment={**os.environ,'SWITCHMUSIC_UI_BACKEND':f'http://127.0.0.1:{server.server_port}'}
            for scene in ((args.scene,) if args.scene else scenes):
                command=[str(binary),scene,'1']
                if output:command.append(str(output/(scene+'.ppm')))
                result=subprocess.run(command,cwd=directory,env=environment,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=40)
                if result.returncode or 'READY '+scene not in result.stdout:
                    raise RuntimeError(f'{scene} failed ({result.returncode}):\n{result.stdout}')
                print(f'{scene}: passed',flush=True)
        finally:server.shutdown();thread.join()
    print('XML inflation, measured layout, long rows, empty states and controller navigation passed')

if __name__=='__main__':main()
