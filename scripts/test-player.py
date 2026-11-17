#!/usr/bin/env python3
"""Execute the production decoder/custom AVIO over authenticated HTTP, without SD audio."""
from array import array
import http.server
import json
import os
import re
import socket
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading
import time

ROOT=Path(__file__).resolve().parents[1]

def run(args,**kwargs):return subprocess.run([str(x) for x in args],check=True,**kwargs)

class Server(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_GET(self):
        if self.headers.get('Cookie')!='switchmusic_session='+'a'*43:
            self.send_error(401);return
        if self.path=='/v1/large-metadata':
            raw=b'x'*(600*1024);self.send_response(200);self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw);return
        if self.path=='/v1/recover':
            raw=b'online';self.send_response(200);self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw);return
        if self.path in ('/v1/stream/recover','/v1/stream/partial'):
            size=65536;self.send_response(206);self.send_header('Content-Range',f'bytes 0-{size-1}/{size}');self.send_header('Content-Length',str(size));self.end_headers()
            raw=bytes(i%251 for i in range(size))
            if self.path.endswith('/partial'):
                self.server.partial_requests+=1;self.wfile.write(raw[:8192]);self.close_connection=True;self.connection.shutdown(socket.SHUT_RDWR);self.connection.close()
            else:self.wfile.write(raw)
            return
        if self.path=='/v1/unavailable':self.server.unavailable_requests+=1;self.send_error(503);return
        if self.path=='/v1/disconnect':
            self.close_connection=True;self.connection.shutdown(socket.SHUT_RDWR);self.connection.close();return
        if self.path=='/v1/stream/large':
            size=3*1024*1024*1024+123;begin,end=map(int,self.headers['Range'][6:].split('-'));end=min(end,size-1)
            self.send_response(206);self.send_header('Content-Range',f'bytes {begin}-{end}/{size}');self.send_header('Content-Length',str(end-begin+1));self.end_headers()
            try:self.wfile.write(bytes(i%251 for i in range(begin,end+1)))
            except (BrokenPipeError,ConnectionResetError):pass
            return
        if self.path=='/v1/stream/chunked':
            self.server.chunked_requests+=1
            self.send_response(200);self.send_header('Transfer-Encoding','chunked');self.end_headers()
            try:
                for offset in range(0,600*1024,8192):
                    raw=bytes(i%251 for i in range(offset,min(offset+8192,600*1024)))
                    self.wfile.write(f'{len(raw):x}\r\n'.encode()+raw+b'\r\n')
                self.wfile.write(b'0\r\n\r\n')
            except (BrokenPipeError,ConnectionResetError):pass
            return
        media=self.server.media;manifest=self.server.manifest
        if self.path.startswith('/v1/playback/'):
            raw=manifest.encode()
            if self.path.endswith('external.m3u8'):raw=raw.replace(b'/v1/stream/123',b'https://other.example/track')
            if self.path.endswith('bad-range.m3u8'):raw=raw.replace(b'/v1/stream/123',b'/v1/stream/200')
            if self.path.endswith('changed.m3u8'):raw=raw.replace(b'/v1/stream/123',b'/v1/stream/201')
            if self.path.endswith('encrypted.m3u8'):raw=raw.replace(b'#EXTM3U',b'#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI="private"')
            if self.path.endswith('crlf.m3u8'):raw=raw.replace(b'\n',b'\r\n')
            if self.path.endswith('large-manifest.m3u8'):raw=raw.replace(b'#EXTM3U\n',b'#EXTM3U\n#'+b'x'*(300*1024)+b'\n')
            if self.path.endswith('expired.m3u8'):self.send_error(401);return
            self.send_response(200);self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw);return
        size=media.stat().st_size
        if self.path=='/v1/stream/200':
            self.send_response(200);self.send_header('Content-Length',str(size));self.end_headers()
            try:self.wfile.write(media.read_bytes())
            except (BrokenPipeError,ConnectionResetError):pass
            return
        assert self.path in ['/v1/stream/123','/v1/stream/201'],self.path
        value=self.headers.get('Range','');assert value.startswith('bytes=')
        begin,end=map(int,value[6:].split('-'));assert 0<end-begin+1<=256*1024
        end=min(end,size-1)
        if self.path.endswith('/201'):end=min(end,begin+4095)
        assert begin<=end
        self.send_response(206);self.send_header('Content-Range',f'bytes {begin}-{end}/{size}');self.send_header('ETag','"fixture-'+str(self.server.ranges)+'"' if self.path.endswith('/201') else '"fixture"');self.send_header('Content-Length',str(end-begin+1));self.end_headers()
        with media.open('rb') as file:file.seek(begin);self.wfile.write(file.read(end-begin+1))
        self.server.ranges+=1

def compare(actual,reference,label):
    a=array('h');a.frombytes(actual.read_bytes());b=array('h');b.frombytes(reference.read_bytes())
    assert len(a)==len(b),(label,len(a),len(b))
    difference=max((abs(x-y) for x,y in zip(a,b)),default=0)
    assert difference<=1,(label,difference)

def check_reconnect(binary):
    for mode in ('text','range','cancel','refused'):
        locked=socket.socket();locked.bind(('127.0.0.1',0));port=locked.getsockname()[1]
        server=http.server.ThreadingHTTPServer(('127.0.0.1',port),Server,bind_and_activate=False)
        ready=threading.Event();thread=None
        if mode in ('text','range'):
            def late_start():
                time.sleep(.5);locked.close();server.server_bind();server.server_activate();ready.set();server.serve_forever()
            thread=threading.Thread(target=late_start,daemon=True);thread.start()
        try:
            run([binary,f'http://127.0.0.1:{port}','--reconnect-'+mode],timeout=20)
        finally:
            if thread:
                assert ready.wait(2),'Delayed server did not start';server.shutdown();thread.join()
            locked.close();server.server_close()

def main():
    prefix=ROOT/'build-ffmpeg/host';out=ROOT/'build-ffmpeg/player-tests';out.mkdir(parents=True,exist_ok=True);binary=out/'player'
    curl=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','libcurl'],text=True))
    guard=out/'heap_guard.o'
    run(['cc','-O2','-c',ROOT/'tests/audio/heap_guard.c','-o',guard])
    run(['c++','-std=c++17','-O2','-I'+str(ROOT/'common/include'),'-I'+str(ROOT/'sysmodule/include'),'-I'+str(ROOT/'third_party/json'),'-I'+str(prefix/'include'),
         ROOT/'tests/audio/player.cpp',ROOT/'sysmodule/source/audio_decoder.cpp',ROOT/'sysmodule/source/http_input.cpp',ROOT/'common/src/client.cpp',
         prefix/'lib/libavformat.a',prefix/'lib/libavcodec.a',prefix/'lib/libswresample.a',prefix/'lib/libavutil.a',*curl,guard,'-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=posix_memalign,--wrap=aligned_alloc','-lm','-pthread','-o',binary])
    check_reconnect(binary)
    with tempfile.TemporaryDirectory(prefix='switchmusic-player-') as temporary:
        work=Path(temporary)
        with http.server.ThreadingHTTPServer(('127.0.0.1',0),Server) as server:
            thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
            try:
                for name,codec,rate,fmt in [('aac','aac',48000,'fltp'),('aac-web','aac',44100,'fltp'),('alac','alac',44100,'s16p'),('alac-hires','alac',192000,'s32p')]:
                    source=work/'source.m4a';playlist=work/'index.m3u8';media=work/'media.m4s'
                    run(['ffmpeg','-v','error','-f','lavfi','-i',f'sine=frequency=997:sample_rate={rate}:duration=4','-ac','2','-c:a',codec,'-sample_fmt',fmt,'-movie_timescale',rate,*(['-b:a','256k'] if codec=='aac' else []),'-y',source])
                    run(['ffmpeg','-v','error','-i',source,'-c:a','copy','-f','hls','-hls_segment_type','fmp4','-hls_flags','single_file','-hls_time','60','-hls_list_size','0','-hls_segment_filename',media,'-y',playlist])
                    server.media=media;server.manifest=playlist.read_text().replace('media.m4s','/v1/stream/123');server.ranges=0
                    actual=work/'actual.pcm';seeked=work/'seek.pcm';middle=work/'middle.pcm';reference=work/'reference.pcm'
                    url='http://127.0.0.1:'+str(server.server_port)
                    measured=run([binary,url,'/v1/playback/123.m3u8',actual,seeked,middle],stderr=subprocess.PIPE)
                    peak=int(re.search(rb'SWITCHMUSIC_HEAP_PEAK=(\d+)',measured.stderr)[1]);assert peak<=8*1024*1024
                    duration=int(json.loads(subprocess.check_output(['ffprobe','-v','error','-show_entries','stream=duration_ts','-of','json',str(media)],text=True))['streams'][0]['duration_ts'])
                    run(['ffmpeg','-v','error','-i',media,'-af',f'atrim=end_sample={duration}','-ac','2','-ar','48000','-f','s16le','-y',reference]);compare(actual,reference,name)
                    full=reference.read_bytes();reference.write_bytes(full[48000*4:]);compare(seeked,reference,name+' seek')
                    reference.write_bytes(full[120000*4:]);compare(middle,reference,name+' middle seek')
                    assert server.ranges>0
                    print(name+f': decoded PCM, seek, EOF and cancellation passed; tracked heap peak {peak} bytes')
                    # Same PCM and seeks through a server/proxy that ignores Range.
                    result=run([binary,url,'/v1/playback/bad-range.m3u8',actual,seeked,middle],stderr=subprocess.PIPE)
                    reference.write_bytes(full);compare(actual,reference,name+' full-response fallback')
                    reference.write_bytes(full[48000*4:]);compare(seeked,reference,name+' full-response seek')
                    reference.write_bytes(full[120000*4:]);compare(middle,reference,name+' full-response middle seek')
                    print(name+': streamed full-response fallback and seeking passed')
                for fixture in ['crlf','large-manifest']:
                    run([binary,url,'/v1/playback/'+fixture+'.m3u8',actual,seeked,middle])
                    reference.write_bytes(full);compare(actual,reference,fixture)
                    print(fixture+': streamed playback passed')
                server.chunked_requests=0;server.unavailable_requests=0;server.partial_requests=0
                run([binary,url,'--http-checks'])
                assert server.chunked_requests==2,server.chunked_requests
                assert server.unavailable_requests==1 and server.partial_requests==1
                for fixture in ['external','expired','changed','encrypted']:
                    result=subprocess.run([str(binary),url,'/v1/playback/'+fixture+'.m3u8',str(actual),str(seeked),str(middle)],capture_output=True)
                    assert result.returncode!=0,fixture
                    print(fixture+': rejected')
            finally:server.shutdown();thread.join()
if __name__=='__main__':main()
