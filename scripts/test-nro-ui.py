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

class Backend(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_GET(self):
        if self.path=='/auth/me':payload={'user':{'username':'demo','apple_linked':True},'csrf':'synthetic-csrf'}
        elif self.path.startswith('/v1/catalog/search?'):
            payload={'items':[{'id':str(100+i),'type':'albums','title':'Album '+str(i+1),'artist':'Artist'} for i in range(12)],'next_offset':12}
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
