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
    files = {}
    for i in range(count):
        offset, size, name_offset = struct.unpack_from('<QQI', data, 16 + 24 * i)
        name = data[strings + name_offset:].split(b'\0', 1)[0].decode()
        files[name] = data[payload + offset:payload + offset + size]
    return files

def validate(build):
    nro = (build / 'SwitchMusic.nro').read_bytes()
    ovl = (build / 'switchmusic.ovl').read_bytes()
    assert nro[16:20] == b'NRO0', 'Invalid NRO header'
    assert ovl[16:20] == b'NRO0' and ovl[-4:] == b'ULTR', 'Invalid overlay/signature'
    entries = pfs_entries((build / 'switchmusic-sys.nsp').read_bytes())
    assert entries['main'][:4] == b'NSO0', 'Invalid sysmodule NSO'
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
    files = {
        'switch/SwitchMusic/SwitchMusic.nro': args.build/'SwitchMusic.nro',
        'switch/.overlays/switchmusic.ovl': args.build/'switchmusic.ovl',
        f'atmosphere/contents/{TITLE:016X}/exefs.nsp': args.build/'switchmusic-sys.nsp',
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
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(args.output, 'w', ZIP_DEFLATED) as archive:
        for name,path in files.items():
            archive.write(path, name)
        archive.writestr('SwitchMusic-SHA256.json', json.dumps(manifest,indent=2)+'\n')
    print(f'Validated NRO, overlay, NSP and program ID; wrote {args.output}')

if __name__ == '__main__':
    main()
