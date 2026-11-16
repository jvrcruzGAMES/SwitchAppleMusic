"""Exercise the production artwork worker and IPC chunk protocol against loopback fixtures."""
import http.server
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading
import unittest
ROOT=Path(__file__).resolve().parents[1]
class ArtworkServer(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_GET(self):
        assert self.headers.get('Cookie')=='switchmusic_session='+'a'*43
        assert self.path.startswith('/v1/artwork/songs/') and self.path.endswith('?size=256&format=rgba4444')
        track=self.path.split('/')[-1].split('?')[0]
        raw=bytes(i%251 for i in range(256*256*2)) if track=='123' else bytes([int(track)])*(256*256*2)
        self.send_response(200);self.send_header('Content-Length',str(len(raw)));self.end_headers()
        try:self.wfile.write(raw)
        except (BrokenPipeError,ConnectionResetError):pass
class ArtworkTest(unittest.TestCase):
    def test_background_worker_and_chunk_assembly(self):
        with tempfile.TemporaryDirectory(prefix='switchmusic-artwork-') as directory:
            binary=Path(directory)/'artwork'
            flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','libcurl'],text=True))
            subprocess.run(['c++','-std=c++17','-I'+str(ROOT/'common/include'),'-I'+str(ROOT/'sysmodule/include'),'-I'+str(ROOT/'third_party/json'),str(ROOT/'tests/artwork.cpp'),str(ROOT/'common/src/client.cpp'),'-o',str(binary),*flags,'-pthread'],check=True)
            with http.server.ThreadingHTTPServer(('127.0.0.1',0),ArtworkServer) as server:
                thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
                try:subprocess.run([str(binary),f'http://127.0.0.1:{server.server_port}'],check=True,timeout=15)
                finally:server.shutdown();thread.join()
