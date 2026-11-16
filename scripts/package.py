#!/usr/bin/env python3
"""Validate native container headers and assemble an unactivated SD archive."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED

ROOT = Path(__file__).resolve().parent.parent
TITLE = 0x420000000000534D

def pfs_entries(data):
    assert data[:4] == b'PFS0', 'Invalid sysmodule PFS0 header'
    count, strings_size = struct.unpack_from('<II', data, 4)
    strings = 16 + 24 * count
    payload = strings + strings_size
    metadata=json.loads((ROOT/'build-ffmpeg/switch/switchmusic-build.json').read_text())
    source=ROOT/'build-ffmpeg'/('source-'+metadata['source_fingerprint'][:12])
    files = {
        'SwitchMusic-LICENSES/FFmpeg-LGPL-2.1.txt': source/'COPYING.LGPLv2.1',
        'SwitchMusic-LICENSES/FFmpeg-LICENSE.md': source/'LICENSE.md',}
    for i in range(count):
        offset, size, name_offset = struct.unpack_from('<QQI', data, 16 + 24 * i)
        name = data[strings + name_offset:].split(b'\0', 1)[0].decode()
        files[name] = data[payload + offset:payload + offset + size]
    return files

def romfs_file(image, name):
    assert len(image)>=80, 'Truncated certificate RomFS'
    header=struct.unpack_from('<10Q',image)
    assert header[0]==80, 'Invalid RomFS header'
    table,end=header[7],header[7]+header[8]
    assert end<=len(image), 'Invalid RomFS file table'
    while table<end:
        _,_,offset,size,_,length=struct.unpack_from('<IIQQII',image,table)
        filename=image[table+32:table+32+length].decode()
        if filename==name:
            start=header[9]+offset
            assert start+size<=len(image), 'Truncated RomFS certificate'
            return image[start:start+size]
        table+=(32+length+3)&~3
    raise AssertionError('Missing bundled '+name)

def validate_certificates(build,nro):
    size=struct.unpack_from('<I',nro,0x18)[0]
    assert nro[size:size+4]==b'ASET', 'Missing NRO assets'
    offset,length=struct.unpack_from('<QQ',nro,size+0x28)
    embedded=nro[size+offset:size+offset+length]
    module=(build/'switchmusic-sys.romfs').read_bytes()
    expected=(ROOT/'resources/certs/cacert.pem').read_bytes()
    assert expected.count(b'-----BEGIN CERTIFICATE-----')>100, 'Incomplete CA trust store'
    for image in (embedded,module):
        assert romfs_file(image,'cacert.pem')==expected, 'Bundled CA trust store differs from source'
        assert romfs_file(image,'LICENSE.txt')==(ROOT/'resources/certs/LICENSE.txt').read_bytes(), 'Missing CA license'

def memory_report(nso):
    # NSO decompressed loadable sections, plus BSS (including the fixed heap).
    align=lambda value:(value+4095)&~4095
    sizes=[struct.unpack_from('<I',nso,offset)[0] for offset in (0x18,0x28,0x38,0x3c)]
    baseline=sum(align(size) for size in sizes)
    reserve=4*1024*1024
    total=baseline+reserve
    assert total<=22000000, f'Sysmodule exceeds 22 MB planning goal: {total} bytes'
    return {'scope':'Actual FFmpeg sysmodule NSO; hardware runtime peak remains unmeasured',
            'loadable_sections_and_bss_bytes':baseline,'reserved_heap_bytes':8*1024*1024,
            'additional_platform_allowance_bytes':reserve,'planning_total_bytes':total,
            'goal_bytes':22000000,'hard_limit_bytes':36600000}

def validate_toolbox(path):
    metadata = json.loads(path.read_text())
    if not isinstance(metadata, dict) or not isinstance(metadata.get('name'), str) or not metadata['name'].strip():
        raise ValueError('Toolbox metadata must include a display name')
    if metadata.get('tid') != f'{TITLE:016X}':
        raise ValueError('Toolbox title ID must match the packaged sysmodule')
    if metadata.get('requires_reboot') is not False:
        raise ValueError('SwitchMusic Toolbox metadata must allow live load/unload')
    return metadata


def validate(build):
    validate_toolbox(ROOT/'sysmodule/toolbox.json')
    nro = (build / 'SwitchMusic.nro').read_bytes()
    ovl = (build / 'switchmusic.ovl').read_bytes()
    assert nro[16:20] == b'NRO0', 'Invalid NRO header'
    validate_certificates(build,nro)
    assert ovl[16:20] == b'NRO0' and ovl[-4:] == b'ULTR', 'Invalid overlay/signature'
    entries = pfs_entries((build / 'switchmusic-sys.nsp').read_bytes())
    assert entries['main'][:4] == b'NSO0', 'Invalid sysmodule NSO'
    memory_report(entries['main'])
    npdm = entries['main.npdm']
    assert npdm[:4] == b'META', 'Invalid NPDM'
    aci_offset = struct.unpack_from('<I', npdm, 0x70)[0]
    assert npdm[aci_offset:aci_offset+4] == b'ACI0', 'Missing NPDM ACI0'
    assert struct.unpack_from('<Q', npdm, aci_offset + 16)[0] == TITLE, 'Incorrect NPDM program ID'

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, default=ROOT/'build-switch')
    p.add_argument('--output', type=Path, default=ROOT/'dist/SwitchMusic-sdcard.zip')
    args = p.parse_args()
    validate(args.build)
    metadata=json.loads((ROOT/'build-ffmpeg/switch/switchmusic-build.json').read_text())
    source=ROOT/'build-ffmpeg'/('source-'+metadata['source_fingerprint'][:12])
    files = {
        'SwitchMusic-LICENSES/FFmpeg-LGPL-2.1.txt': source/'COPYING.LGPLv2.1',
        'SwitchMusic-LICENSES/FFmpeg-LICENSE.md': source/'LICENSE.md',
        'switch/SwitchMusic/SwitchMusic.nro': args.build/'SwitchMusic.nro',
        'switch/.overlays/switchmusic.ovl': args.build/'switchmusic.ovl',
        f'atmosphere/contents/{TITLE:016X}/exefs.nsp': args.build/'switchmusic-sys.nsp',
        f'atmosphere/contents/{TITLE:016X}/romfs.bin': args.build/'switchmusic-sys.romfs',
        'SwitchMusic-LICENSES/mozilla-ca.txt': ROOT/'resources/certs/LICENSE.txt',
        f'atmosphere/contents/{TITLE:016X}/toolbox.json': ROOT/'sysmodule/toolbox.json',
        'config/switchmusic/config.example.json': ROOT/'config/config.example.json',
        'SwitchMusic-README.md': ROOT/'README.md',
        'SwitchMusic-VALIDATION.md': ROOT/'docs/VALIDATION.md',
        'SwitchMusic-LICENSE.txt': ROOT/'LICENSE',
        'SwitchMusic-THIRD_PARTY_NOTICES.md': ROOT/'THIRD_PARTY_NOTICES.md',
        'SwitchMusic-LICENSES/borealis.txt': ROOT/'third_party/borealis/LICENSE',
        'SwitchMusic-LICENSES/libultrahand.txt': ROOT/'third_party/libultrahand/LICENSE',
        'SwitchMusic-LICENSES/libultrahand-sub.txt': ROOT/'third_party/libultrahand/SUB_LICENSE',
        'SwitchMusic-LICENSES/nlohmann-json.txt': ROOT/'third_party/json/LICENSE.MIT',
    }
    manifest = {name:hashlib.sha256(path.read_bytes()).hexdigest() for name,path in files.items()}
    memory=json.dumps(memory_report(pfs_entries((args.build/'switchmusic-sys.nsp').read_bytes())['main']),indent=2)+'\n'
    manifest['SwitchMusic-memory.json']=hashlib.sha256(memory.encode()).hexdigest()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(args.output, 'w', ZIP_DEFLATED) as archive:
        # Keep the full install tree even while boot activation remains opt-in.
        archive.writestr(f'atmosphere/contents/{TITLE:016X}/flags/', b'')
        for name,path in files.items():
            archive.write(path, name)
        archive.writestr('SwitchMusic-memory.json',memory)
        archive.writestr('SwitchMusic-SHA256.json', json.dumps(manifest,indent=2)+'\n')
    print(f'Validated NRO, overlay, NSP and program ID; wrote {args.output}')

if __name__ == '__main__':
    main()
