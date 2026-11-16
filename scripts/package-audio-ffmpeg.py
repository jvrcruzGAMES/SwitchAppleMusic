#!/usr/bin/env python3
"""Package the isolated decoder SDK, measured reports and corresponding source."""
import argparse
import hashlib
import json
from pathlib import Path
import tarfile

ROOT=Path(__file__).resolve().parent.parent

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-only',action='store_true',help='Package corresponding patched source without SDK/test reports')
    p.add_argument('--root',type=Path,default=ROOT/'build-ffmpeg')
    p.add_argument('--output',type=Path,default=ROOT/'dist')
    args=p.parse_args();work=args.root.resolve();prefix=work/'switch';out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    metadata=json.loads((prefix/'switchmusic-build.json').read_text())
    source=work/('source-'+metadata['source_fingerprint'][:12])
    binary=out/'switch-ffmpeg-audio-7.1-switchmusic.tar.gz'
    if not args.source_only:
        report=json.loads((work/'budget/memory-report.json').read_text())
        if not report['passes_goal']:raise SystemExit('Budget check must pass before packaging')
        with tarfile.open(binary,'w:gz') as archive:
            archive.add(prefix,arcname='switch-ffmpeg-audio')
            archive.add(work/'budget/memory-report.json',arcname='switch-ffmpeg-audio/memory-report.json')
            archive.add(work/'host-tests/decode-report.json',arcname='switch-ffmpeg-audio/decode-report.json')
            archive.add(ROOT/'docs/AUDIO_FFMPEG.md',arcname='switch-ffmpeg-audio/README.md')
            archive.add(ROOT/'toolchain/AudioFFmpeg.cmake',arcname='switch-ffmpeg-audio/AudioFFmpeg.cmake')
            archive.add(ROOT/'toolchain/patches',arcname='switch-ffmpeg-audio/recipe/toolchain/patches')
            archive.add(ROOT/'scripts/build-audio-ffmpeg.py',arcname='switch-ffmpeg-audio/recipe/scripts/build-audio-ffmpeg.py')
            for name in ['COPYING.LGPLv2.1','COPYING.LGPLv3','COPYING.GPLv2','COPYING.GPLv3','LICENSE.md']:
                archive.add(source/name,arcname='switch-ffmpeg-audio/licenses/'+name)
    source_archive=out/'switch-ffmpeg-audio-7.1-source.tar.xz'
    with tarfile.open(source_archive,'w:xz') as archive:
        archive.add(source,arcname='ffmpeg-7.1-switchmusic')
        archive.add(ROOT/'toolchain/patches',arcname='recipe/toolchain/patches')
        archive.add(ROOT/'scripts/build-audio-ffmpeg.py',arcname='recipe/scripts/build-audio-ffmpeg.py')
        archive.add(ROOT/'docs/AUDIO_FFMPEG.md',arcname='recipe/README.md')
    for path in ([source_archive] if args.source_only else [binary,source_archive]):
        digest=hashlib.sha256(path.read_bytes()).hexdigest()
        path.with_suffix(path.suffix+'.sha256').write_text(f'{digest}  {path.name}\n')
        print(path)

if __name__=='__main__':main()
