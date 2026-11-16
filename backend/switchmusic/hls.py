"""Single-file, byte-range fMP4 VOD; no credentials appear in its URLs."""
import math
import struct
from fastapi import HTTPException

def playlist(path,song_id,duration):
    size=path.stat().st_size
    offset=0;initialized=False;media=None
    with path.open('rb') as source:
        # Read box headers only; never load a complete song into Python memory.
        while offset<size:
            source.seek(offset);header=source.read(8)
            if len(header)!=8:break
            length,kind=struct.unpack('>I4s',header)
            minimum=8
            if length==1:
                extended=source.read(8)
                if len(extended)!=8:break
                length=struct.unpack('>Q',extended)[0];minimum=16
            if length==0:length=size-offset
            if length<minimum or offset+length>size:break
            if kind==b'moov':initialized=True
            if kind==b'moof':media=offset;break
            offset+=length
    if not initialized or media is None or media<=0 or media>=size:
        raise HTTPException(502,'Prepared audio is not fragmented MP4')
    if not math.isfinite(duration) or duration<=0:raise HTTPException(502,'Invalid track duration')
    uri=f'/v1/stream/{song_id}'
    return (f'#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:{math.ceil(duration)}\n'
            '#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-PLAYLIST-TYPE:VOD\n'
            f'#EXT-X-MAP:URI="{uri}",BYTERANGE="{media}@0"\n'
            f'#EXTINF:{duration:.6f},\n#EXT-X-BYTERANGE:{size-media}@{media}\n{uri}\n#EXT-X-ENDLIST\n')
