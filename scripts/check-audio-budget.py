#!/usr/bin/env python3
"""Cross-link the audio/HTTP/HOS probe and enforce conservative decimal MB budgets."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

ROOT=Path(__file__).resolve().parent.parent

def run(args,**kwargs):return subprocess.run([str(x) for x in args],check=True,**kwargs)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--prefix',type=Path,default=ROOT/'build-ffmpeg/switch')
    p.add_argument('--output',type=Path,default=ROOT/'build-ffmpeg/budget')
    args=p.parse_args();prefix=args.prefix.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    dkp=Path(os.environ.get('DEVKITPRO','/opt/devkitpro'));cc=dkp/'devkitA64/bin/aarch64-none-elf-gcc';cxx=dkp/'devkitA64/bin/aarch64-none-elf-g++'
    env=os.environ.copy();env['DEVKITPRO']=str(dkp)
    flags=['-D_GNU_SOURCE','-D__SWITCH__','-DCURL_STATICLIB','-Os','-fPIE','-ffunction-sections','-fdata-sections','-march=armv8-a+crc+crypto','-mtune=cortex-a57','-mtp=soft','-ftls-model=local-exec',f'-I{prefix}/include',f'-I{dkp}/libnx/include',f'-I{dkp}/portlibs/switch/include']
    run([cc,*flags,'-DSWITCHMUSIC_BUDGET_PROBE','-c',ROOT/'tests/audio/decode.c','-o',out/'decode.o'],env=env)
    includes=[f'-I{ROOT}/common/include',f'-I{ROOT}/third_party/json']
    objects=[]
    for source,name in [(ROOT/'tests/audio/switch_budget.cpp','budget'),(ROOT/'common/src/client.cpp','client')]:
        obj=out/(name+'.o');objects.append(obj)
        run([cxx,'-std=c++17',*flags,*includes,'-c',source,'-o',obj],env=env)
    elf=out/'switchmusic-audio-budget.elf'
    run([cxx,*flags,f'-L{dkp}/libnx/lib',f'-L{dkp}/portlibs/switch/lib',f'-specs={dkp}/libnx/switch.specs',
         '-Wl,--gc-sections',f'-Wl,-Map,{out}/audio.map',out/'decode.o',*objects,
         prefix/'lib/libavformat.a',prefix/'lib/libavcodec.a',prefix/'lib/libswresample.a',prefix/'lib/libavutil.a',
         '-lcurl','-lz','-lnx','-lm','-pthread','-o',elf],env=env)
    size=run([dkp/'devkitA64/bin/aarch64-none-elf-size',elf],capture_output=True,text=True).stdout
    print(size,end='');text,data,bss=map(int,size.splitlines()[1].split()[:3])
    # Align loadable sections to pages rather than equating file size with RAM.
    align=lambda value:(value+4095)//4096*4096
    baseline=sum(map(align,[text,data,bss]))
    reserve=4*1024*1024  # extra platform allocations beyond the reserved 8 MiB heap
    budget=baseline+reserve
    config=(prefix/'config_components.h').read_text()
    decoders=re.findall(r'^#define CONFIG_(\w+)_DECODER 1$',config,re.M)
    assert sorted(decoders)==['AAC','ALAC'],f'Unexpected decoders: {decoders}'
    nm=run([dkp/'devkitA64/bin/aarch64-none-elf-nm',elf],capture_output=True,text=True).stdout
    assert not re.search(r'ff_tx_tab_(262144|524288|1048576|2097152)_',nm),'Oversized FFT tables remain'
    assert 'mpv_' not in nm,'mpv accidentally linked'
    report={'measured_linked_bytes':{'text_rodata':text,'data':data,'bss_including_heap':bss,'page_aligned_total':baseline},
            'reserved_heap_bytes':8*1024*1024,'additional_platform_allowance_bytes':reserve,
            'planning_total_bytes':budget,'goal_bytes':22000000,'hard_limit_bytes':36600000,
            'passes_goal':budget<=22000000,'passes_hard_limit':budget<=36600000,
            'scope':'Decoder + existing HTTP/control code + HOS audio linkage; isolated SDK probe, not the production player or a hardware runtime measurement'}
    (out/'memory-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    if budget>22000000:raise SystemExit('Audio profile exceeds the 22 MB planning budget')

if __name__=='__main__':main()
