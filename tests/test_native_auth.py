"""Exercise the actual native libcurl client against a loopback session server."""
import http.server
import json
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading
import unittest

ROOT=Path(__file__).resolve().parents[1]

class SessionServer(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def reply(self,status,payload,cookie=None):
        raw=b'' if status==204 else json.dumps(payload).encode();self.send_response(status)
        self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)))
        if cookie:self.send_header('Set-Cookie',cookie)
        self.end_headers();self.wfile.write(raw)
    def authorized(self):return self.server.session and self.headers.get('Cookie')=='switchmusic_session='+self.server.session
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers.get('Content-Length',0))) or '{}')
        if self.path=='/auth/login':
            if body!={'username':'admin','password':'Synthetic-password-12345'}:return self.reply(401,{'detail':'Invalid username or password'})
            self.server.session='a'*43
            return self.reply(200,{'user':{'username':'admin'},'csrf':'csrf-value'},'switchmusic_session='+self.server.session+'; HttpOnly; Path=/; SameSite=Strict')
        if self.path=='/auth/apple/qr/start' and self.authorized() and self.headers.get('X-CSRF-Token')=='csrf-value':
            return self.reply(200,{'confirmation':'123-456'})
        if self.path=='/auth/logout' and self.authorized() and self.headers.get('X-CSRF-Token')=='csrf-value':
            self.server.session=None;return self.reply(204,None)
        self.reply(403,{'detail':'Denied'})
    def do_GET(self):
        if not self.authorized():return self.reply(401,{'detail':'Sign in required'})
        if self.path=='/test/expire':self.server.session=None
        self.reply(200,{'user':{'username':'admin'},'csrf':'csrf-value'})

class NativeAuthTest(unittest.TestCase):
    def test_native_client_sessions(self):
        with tempfile.TemporaryDirectory(prefix='switchmusic-native-auth-') as temporary:
            binary=Path(temporary)/'native-auth'
            flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','libcurl'],text=True))
            subprocess.run(['c++','-std=c++17','-I'+str(ROOT/'common/include'),'-I'+str(ROOT/'third_party/json'),str(ROOT/'common/src/client.cpp'),str(ROOT/'tests/native_auth.cpp'),'-o',str(binary),*flags,'-pthread','-Wl,--wrap=rename'],check=True)
            with http.server.ThreadingHTTPServer(('127.0.0.1',0),SessionServer) as server:
                server.session=None
                thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
                try:subprocess.run([str(binary),'http://127.0.0.1:'+str(server.server_port),str(Path(temporary)/'config.json')],check=True,timeout=30)
                finally:server.shutdown();thread.join()

if __name__=='__main__':unittest.main()
