from pathlib import Path
import http.server
import json
import shlex
import subprocess
import tempfile
import threading
import unittest
ROOT=Path(__file__).resolve().parents[1]
class Backend(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_GET(self):
        assert self.path=='/v1/lyrics/123'
        assert self.headers['Cookie']=='switchmusic_session='+'a'*43
        raw=json.dumps({'lines':[{'time_ms':1000*(i+1),'text':str(i)} for i in range(6)]}).encode()
        self.send_response(200);self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
class LyricsTest(unittest.TestCase):
    def test_five_lines_and_background_worker(self):
        with tempfile.TemporaryDirectory(prefix='switchmusic-lyrics-') as directory:
            binary=Path(directory)/'lyrics'
            flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','libcurl'],text=True))
            subprocess.run(['c++','-std=c++17','-I'+str(ROOT/'common/include'),'-I'+str(ROOT/'sysmodule/include'),'-I'+str(ROOT/'third_party/json'),str(ROOT/'tests/lyrics.cpp'),str(ROOT/'common/src/client.cpp'),'-o',str(binary),*flags,'-pthread'],check=True)
            with http.server.ThreadingHTTPServer(('127.0.0.1',0),Backend) as server:
                thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
                try:subprocess.run([str(binary),f'http://127.0.0.1:{server.server_port}'],check=True,timeout=10)
                finally:server.shutdown();thread.join()
