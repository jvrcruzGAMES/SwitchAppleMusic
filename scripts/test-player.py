#!/usr/bin/env python3
"""Execute the production decoder/custom AVIO over authenticated HTTP, without SD audio."""
from array import array
import http.server
import json
import os
import re
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading

ROOT=Path(__file__).resolve().parents[1]

def run(args,**kwargs):return subprocess.run([str(x) for x in args],check=True,**kwargs)

class Server(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_GET(self):
        if self.headers.get('Cookie')!='switchmusic_session='+'a'*43:
            self.send_error(401);return
        media=self.server.media;manifest=self.server.manifest
        if self.path.startswith('/v1/playback/'):
            raw=manifest.encode()
            if self.path.endswith('external.m3u8'):raw=raw.replace(b'/v1/stream/123',b'https://other.example/track')
            if self.path.endswith('bad-range.m3u8'):raw=raw.replace(b'/v1/stream/123',b'/v1/stream/200')
            if self.path.endswith('changed.m3u8'):raw=raw.replace(b'/v1/stream/123',b'/v1/stream/201')
            if self.path.endswith('encrypted.m3u8'):raw=raw.replace(b'#EXTM3U',b'#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI="private"')
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

def main():
    prefix=ROOT/'build-ffmpeg/host';out=ROOT/'build-ffmpeg/player-tests';out.mkdir(parents=True,exist_ok=True);binary=out/'player'
    curl=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','libcurl'],text=True))
    guard=out/'heap_guard.o'
    run(['cc','-O2','-c',ROOT/'tests/audio/heap_guard.c','-o',guard])
    run(['c++','-std=c++17','-O2','-I'+str(ROOT/'common/include'),'-I'+str(ROOT/'sysmodule/include'),'-I'+str(ROOT/'third_party/json'),'-I'+str(prefix/'include'),
         ROOT/'tests/audio/player.cpp',ROOT/'sysmodule/source/audio_decoder.cpp',ROOT/'sysmodule/source/http_input.cpp',ROOT/'common/src/client.cpp',
         prefix/'lib/libavformat.a',prefix/'lib/libavcodec.a',prefix/'lib/libswresample.a',prefix/'lib/libavutil.a',*curl,guard,'-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=posix_memalign,--wrap=aligned_alloc','-lm','-pthread','-o',binary])
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
                for fixture in ['external','bad-range','expired','changed','encrypted']:
                    result=subprocess.run([str(binary),url,'/v1/playback/'+fixture+'.m3u8',str(actual),str(seeked),str(middle)],capture_output=True)
                    assert result.returncode!=0,fixture
                    print(fixture+': rejected')
            finally:server.shutdown();thread.join()
if __name__=='__main__':main()
