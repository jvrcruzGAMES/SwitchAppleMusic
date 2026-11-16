#!/usr/bin/env python3
"""Build isolated AAC/ALAC FFmpeg archives; never overwrite devkitPro portlibs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

ROOT=Path(__file__).resolve().parent.parent
VERSION='7.1'
SHA256='40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6'
PORT_PATCH_SHA256='1792380b992e3554a4abcddf0d7b395bfd8c118ac7c6e38c8f2fb0d39753a390'

def run(args,**kwargs):
    subprocess.run([str(a) for a in args],check=True,**kwargs)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--target',choices=['switch','host'],default='switch')
    p.add_argument('--root',type=Path,default=ROOT/'build-ffmpeg')
    p.add_argument('--jobs',type=int,default=min(4,os.cpu_count() or 2))
    args=p.parse_args()
    work=args.root.resolve();work.mkdir(parents=True,exist_ok=True)
    archive=work/f'ffmpeg-{VERSION}.tar.xz'
    if not archive.exists():
        with urllib.request.urlopen(f'https://ffmpeg.org/releases/{archive.name}',timeout=60) as response,archive.open('wb') as output:
            shutil.copyfileobj(response,output)
    if hashlib.sha256(archive.read_bytes()).hexdigest()!=SHA256:
        raise SystemExit('FFmpeg archive SHA-256 mismatch')
    patches=[ROOT/'toolchain/patches/ffmpeg-7.1-switch.patch',ROOT/'toolchain/patches/ffmpeg-7.1-audio-tables.patch']
    if hashlib.sha256(patches[0].read_bytes()).hexdigest()!=PORT_PATCH_SHA256:
        raise SystemExit('devkitPro port patch SHA-256 mismatch')
    fingerprint=hashlib.sha256(archive.read_bytes()+b''.join(x.read_bytes() for x in patches)).hexdigest()
    source=work/f'source-{fingerprint[:12]}'
    if not source.exists():
        temporary=work/f'extract-{fingerprint[:12]}'
        temporary.mkdir(exist_ok=True)
        with tarfile.open(archive) as t:
            t.extractall(temporary,filter='data')
        extracted=temporary/f'ffmpeg-{VERSION}'
        for patch in patches:
            with patch.open('rb') as data:run(['patch','-p1'],cwd=extracted,stdin=data)
        extracted.rename(source);temporary.rmdir()
    prefix=work/args.target
    build=work/f'objects-{args.target}';build.mkdir(exist_ok=True)
    flags=['--disable-everything','--disable-autodetect','--disable-programs',
           '--disable-doc','--disable-debug','--disable-shared','--enable-static',
           '--enable-small','--enable-pic','--disable-avdevice','--disable-avfilter',
           '--disable-swscale','--disable-postproc','--disable-network','--disable-iconv',
           '--disable-symver','--enable-avcodec','--enable-avformat','--enable-avutil',
           '--enable-swresample','--enable-decoder=aac,alac',
           '--enable-parser=aac','--enable-demuxer=mov,aac,hls',
           '--enable-protocol=file','--disable-runtime-cpudetect',
           f'--prefix={prefix}']
    env=os.environ.copy()
    common='-fPIC -Os -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables'
    if args.target=='switch':
        dkp=Path(env.get('DEVKITPRO','/opt/devkitpro')).resolve()
        env['DEVKITPRO']=str(dkp)
        env['PATH']=f'{dkp}/devkitA64/bin:'+env['PATH']
        abi='-D__SWITCH__ -D_GNU_SOURCE -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec'
        flags+=['--enable-cross-compile','--cross-prefix=aarch64-none-elf-',
                '--arch=aarch64','--cpu=cortex-a57','--target-os=horizon',
                '--enable-asm','--enable-neon',
                f'--pkg-config={dkp}/portlibs/switch/bin/aarch64-none-elf-pkg-config',
                f'--extra-cflags={common} {abi} -I{dkp}/libnx/include',
                f'--extra-ldflags=-fPIE -L{dkp}/libnx/lib -specs={dkp}/libnx/switch.specs',
                '--extra-libs=-lnx -lm']
    else:
        # Same codec/table configuration; host executables validate actual decoding.
        flags+=['--disable-x86asm',f'--extra-cflags={common}']
    run([source/'configure',*flags],cwd=build,env=env)
    run(['make',f'-j{args.jobs}'],cwd=build,env=env)
    run(['make','install'],cwd=build,env=env)
    metadata={'ffmpeg':VERSION,'archive_sha256':SHA256,'source_fingerprint':fingerprint,
              'target':args.target,'decoders':['aac','alac'],'max_transform_length':131072,
              'configure':flags,'port_patch_commit':'5c46517a8a1a1f0939a90b8068655ea00fa125b9'}
    (prefix/'switchmusic-build.json').write_text(json.dumps(metadata,indent=2)+'\n')
    for name in ['config.h','config_components.h']:
        shutil.copy2(build/name,prefix/name)
    print(f'Installed isolated {args.target} FFmpeg in {prefix}')

if __name__=='__main__':main()
