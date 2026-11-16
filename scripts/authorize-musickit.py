#!/usr/bin/env python3
"""Authorize MusicKit in a local browser; persist only the user token on this host."""
import argparse
import hmac
import json
import os
from pathlib import Path
import secrets
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
import jwt

HTML = '''<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>SwitchMusic · Connect Apple Music</title>
<style>body{font:20px system-ui;max-width:620px;margin:12vh auto;padding:24px;background:#15151b;color:#fff}button{font:inherit;background:#fa375a;color:white;border:0;border-radius:10px;padding:16px}p{line-height:1.6}</style>
<h1>Connect your Apple Music library</h1><p>Authorize the same Apple Music account you use with wrapper-v2. Your Music User Token is saved on this computer. Your Switch receives only your backend device token.</p>
<button id="connect" disabled>Loading MusicKit…</button><p id="status"></p>
<script>
document.addEventListener('musickitloaded',async()=>{
 try {
  await MusicKit.configure({developerToken:DEVELOPER_TOKEN,app:{name:'SwitchMusic',build:'0.1.0'}});
  const button=document.getElementById('connect');button.disabled=false;button.textContent='Authorize Apple Music';
  button.onclick=async()=>{button.disabled=true;try{
   const token=await MusicKit.getInstance().authorize();
   const r=await fetch('/token',{method:'POST',headers:{'Content-Type':'application/json','X-SwitchMusic-CSRF':CSRF_TOKEN},body:JSON.stringify({token})});
   if(!r.ok)throw Error('Token could not be saved');
   document.getElementById('status').textContent='Connected. You can close this tab and start your backend.';
  }catch(e){document.getElementById('status').textContent='Authorization failed. You can try again.';button.disabled=false;}};
 }catch(e){document.getElementById('status').textContent='MusicKit could not initialize. Check your Media Services key.';}
});
</script><script src="https://js-cdn.music.apple.com/musickit/v3/musickit.js"></script></html>'''

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--key',type=Path,default=Path('secrets/AuthKey.p8'))
    p.add_argument('--team-id',default=os.environ.get('APPLE_TEAM_ID'))
    p.add_argument('--key-id',default=os.environ.get('APPLE_KEY_ID'))
    p.add_argument('--output',type=Path,default=Path('secrets/music_user_token'))
    p.add_argument('--port',type=int,default=8765)
    args=p.parse_args()
    if not args.team_id or not args.key_id:
        p.error('Supply --team-id and --key-id, or export APPLE_TEAM_ID and APPLE_KEY_ID')
    now=int(time.time())
    token=jwt.encode({'iss':args.team_id,'iat':now,'exp':now+900},args.key.read_text(),algorithm='ES256',headers={'kid':args.key_id})
    csrf=secrets.token_urlsafe(32)
    page=HTML.replace('DEVELOPER_TOKEN',json.dumps(token)).replace('CSRF_TOKEN',json.dumps(csrf)).encode()
    origin=f'http://127.0.0.1:{args.port}'
    class Handler(BaseHTTPRequestHandler):
        saved=False
        def log_message(self,*args):
            pass
        def respond(self,status,body=b'',kind='text/plain'):
            self.send_response(status);self.send_header('Content-Type',kind);self.send_header('Cache-Control','no-store');self.send_header('X-Content-Type-Options','nosniff');self.end_headers();self.wfile.write(body)
        def do_GET(self):
            if self.headers.get('Host')!=f'127.0.0.1:{args.port}' or self.path!='/':
                self.respond(404);return
            self.respond(200,page,'text/html; charset=utf-8')
        def do_POST(self):
            if self.path!='/token' or self.headers.get('Origin')!=origin or not hmac.compare_digest(self.headers.get('X-SwitchMusic-CSRF',''),csrf):
                self.respond(403);return
            try:
                size=int(self.headers.get('Content-Length','0'))
                if not 0<size<=32768:
                    self.respond(413);return
                value=json.loads(self.rfile.read(size))['token']
                if not isinstance(value,str) or not value or '\n' in value or '\r' in value:
                    self.respond(400);return
                args.output.parent.mkdir(parents=True,exist_ok=True)
                fd=os.open(args.output,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600)
                with os.fdopen(fd,'w') as f:
                    os.fchmod(f.fileno(),0o600);f.write(value)
                Handler.saved=True;self.respond(200,b'Connected')
            except (OSError,ValueError,KeyError,TypeError):
                self.respond(400)
    server=HTTPServer(('127.0.0.1',args.port),Handler)
    server.timeout=1
    print(f'Open {origin} in a browser on this computer (or use an SSH local port forward).')
    print('Waiting up to 15 minutes for Apple Music authorization…')
    deadline=time.monotonic()+900
    try:
        while not Handler.saved and time.monotonic()<deadline:
            server.handle_request()
    finally:
        server.server_close()
    if not Handler.saved:
        raise SystemExit('Authorization timed out; run the helper again.')
    print(f'Saved Music User Token to {args.output}; keep it on the backend host.')

if __name__=='__main__':
    main()
