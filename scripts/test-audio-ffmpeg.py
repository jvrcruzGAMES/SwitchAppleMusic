#!/usr/bin/env python3
"""Decode AAC/ALAC fixtures with the exact minimal profile and compare PCM."""
from array import array
import argparse
import json
import os
import re
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parent.parent

def run(args,**kwargs):return subprocess.run([str(x) for x in args],check=True,**kwargs)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--prefix',type=Path,default=ROOT/'build-ffmpeg/host')
    p.add_argument('--ffmpeg',default='ffmpeg')
    p.add_argument('--output',type=Path,default=ROOT/'build-ffmpeg/host-tests')
    args=p.parse_args();prefix=args.prefix.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    binary=out/'decode'
    run([os.environ.get('CC','cc'),'-O2','-ffunction-sections','-fdata-sections',f'-I{prefix}/include',ROOT/'tests/audio/decode.c',
         ROOT/'tests/audio/heap_guard.c',
         '-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=posix_memalign,--wrap=aligned_alloc',
         prefix/'lib/libavformat.a',prefix/'lib/libavcodec.a',prefix/'lib/libswresample.a',prefix/'lib/libavutil.a',
         '-Wl,--gc-sections','-lm','-pthread','-o',binary])
    codecs=run([binary,'--codecs'],capture_output=True,text=True).stdout.splitlines()
    assert sorted(codecs)==['aac','alac'],codecs
    run([binary,'--transforms'])
    results=[]
    with tempfile.TemporaryDirectory(prefix='switchmusic-codecs-') as temp:
        temp=Path(temp)
        cases=[('aac-48000','aac',48000,'fltp',False),
               ('aac-web-44100','aac',44100,'fltp',False),
               ('aac-fmp4','aac',48000,'fltp',True),
               ('alac-16-44100','alac',44100,'s16p',False),
               ('alac-24-192000','alac',192000,'s32p',False),
               ('alac-fmp4','alac',96000,'s32p',True)]
        for name,codec,rate,sample_format,fragmented in cases:
            media=temp/(name+'.m4a');reference=temp/'reference.pcm';actual=temp/'actual.pcm'
            command=[args.ffmpeg,'-v','error','-f','lavfi','-i',f'sine=frequency=997:sample_rate={rate}:duration=3',
                     '-af','atrim=end_sample=98304','-ac','2','-c:a',codec,'-sample_fmt',sample_format]
            if codec=='aac':command+=['-b:a','256k']
            if fragmented:command+=['-movflags','frag_keyframe+empty_moov','-frag_duration','500000']
            run([*command,'-y',media])
            run([args.ffmpeg,'-v','error','-i',media,'-ac','2','-ar','48000','-c:a','pcm_s16le','-f','s16le','-y',reference])
            with actual.open('wb') as output:
                measured=run([binary,media],stdout=output,stderr=subprocess.PIPE,text=False)
            heap_peak=int(re.search(rb'SWITCHMUSIC_HEAP_PEAK=(\d+)',measured.stderr)[1])
            assert heap_peak<=8*1024*1024
            a=array('h');a.frombytes(actual.read_bytes());b=array('h');b.frombytes(reference.read_bytes())
            assert len(a)==len(b),(name,len(a),len(b))
            difference=max(abs(x-y) for x,y in zip(a,b))
            # Reference can be a newer FFmpeg; permit one 16-bit quantization step.
            assert difference<=1,(name,difference)
            results.append({'fixture':name,'pcm_bytes':actual.stat().st_size,'max_sample_difference':difference,'tracked_heap_peak_bytes':heap_peak})
        # The enabled HLS demuxer must work with AAC and ALAC fMP4 segments.
        for codec in ['aac','alac']:
            playlist=temp/(codec+'.m3u8')
            run([args.ffmpeg,'-v','error','-f','lavfi','-i','sine=sample_rate=48000:duration=2','-ac','2','-c:a',codec,
                 '-f','hls','-hls_segment_type','fmp4','-hls_time','0.5','-hls_list_size','0',
                 '-hls_fmp4_init_filename',codec+'-init.mp4','-hls_segment_filename',temp/(codec+'-%d.m4s'),'-y',playlist])
            with (temp/'hls.pcm').open('wb') as output:run([binary,playlist],stdout=output)
            assert (temp/'hls.pcm').stat().st_size>48000*4
            results.append({'fixture':codec+'-hls-fmp4','pcm_bytes':(temp/'hls.pcm').stat().st_size})
        unsupported=temp/'unsupported.ac3'
        run([args.ffmpeg,'-v','error','-f','lavfi','-i','sine=duration=0.1','-c:a','ac3','-y',unsupported])
        result=subprocess.run([str(binary),str(unsupported)],capture_output=True)
        assert result.returncode!=0,'AC3 unexpectedly decoded'
    (out/'decode-report.json').write_text(json.dumps({'decoders':codecs,'fixtures':results,'ac3_rejected':True},indent=2)+'\n')
    print(json.dumps(results,indent=2))

if __name__=='__main__':main()
