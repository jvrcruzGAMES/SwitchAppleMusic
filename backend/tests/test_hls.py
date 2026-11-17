import asyncio
from dataclasses import replace
from pathlib import Path
import shutil
import struct
import subprocess
import pytest
from fastapi import HTTPException
from switchmusic.hls import playlist
from switchmusic.playback import Playback
from test_backend import settings,client,HEADERS,SONG

def box(kind,payload=b''):return struct.pack('>I4s',len(payload)+8,kind)+payload

def test_manifest_contains_static_public_backend_relative_ranges(client,monkeypatch):
    async def song(*args):return {**SONG,'attributes':{**SONG['attributes'],'durationInMillis':3500}}
    monkeypatch.setattr("switchmusic.apple.Apple.catalog_song",song)
    async def render(song_id,target):target.write_bytes(box(b'ftyp')+box(b'moov')+box(b'moof')+box(b'mdat',b'audio'))
    client.app.state.playback.render=render
    response=client.get('/v1/playback/123.m3u8')
    assert response.status_code==200,response.text
    assert '#EXT-X-TARGETDURATION:4\n' in response.text
    assert 'BYTERANGE="16@0"' in response.text
    assert '#EXT-X-BYTERANGE:21@16' in response.text
    assert '/v1/stream/123' in response.text and 'http' not in response.text
    assert client.app.state.playback.active=={}
    assert client.get('/v1/stream/123',headers={'Range':'bytes=16-36'}).content==box(b'moof')+box(b'mdat',b'audio')

def test_manifest_rejects_unfragmented_or_invalid_audio(tmp_path):
    media=tmp_path/'audio.m4a'
    for raw in (b'invalid',box(b'ftyp')+box(b'moov')+box(b'mdat',b'data'),box(b'moof')):
        media.write_bytes(raw)
        with pytest.raises(HTTPException):playlist(media,'123',3.5)

@pytest.mark.skipif(not shutil.which('ffmpeg'),reason='Host FFmpeg required')
@pytest.mark.parametrize('codec',['aac','alac'])
async def test_fragmentation_preserves_codec_and_bounded_cache(settings,codec):
    cache=Playback(settings,None);media=settings.cache_dir/'123.part'
    subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','sine=sample_rate=48000:duration=1','-ac','2','-c:a',codec,'-f','mp4','-y',str(media)],check=True)
    await cache.fragment(media)
    manifest=playlist(media,'123',1)
    assert '#EXT-X-MAP' in manifest
    info=subprocess.check_output(['ffprobe','-v','error','-show_entries','stream=codec_name','-of','csv=p=0',str(media)],text=True)
    assert info.strip()==codec
    assert not media.with_suffix('.fragmented').exists()

async def test_fragmentation_checks_transient_space(settings):
    cache=Playback(replace(settings,max_cache_bytes=10,max_track_bytes=8),None)
    media=settings.cache_dir/'123.part';media.write_bytes(b'12345')
    with pytest.raises(HTTPException) as error:await cache.fragment(media)
    assert error.value.status_code==503

async def test_cache_access_does_not_change_media_etag(settings):
    import os,time
    cache=Playback(settings,None)
    async def render(song_id,target):target.write_bytes(b'123456')
    cache.render=render
    path=await cache.materialize('123')
    old=time.time()-settings.cache_ttl-10;os.utime(path,(old,old))
    before=path.stat()
    cache.prune();assert path.exists()  # access clock is newer than file creation
    assert await cache.materialize('123')==path
    assert path.stat().st_mtime_ns==before.st_mtime_ns

async def test_cancelled_remux_kills_child_and_cleans_temp(settings,monkeypatch):
    cache=Playback(settings,None);media=settings.cache_dir/'123.part';media.write_bytes(b'input')
    class Child:
        returncode=None
        def __init__(self):self.done=asyncio.Event();self.killed=False
        async def wait(self):await self.done.wait();return self.returncode
        def kill(self):self.killed=True;self.returncode=-9;self.done.set()
    child=Child()
    async def spawn(*args,**kwargs):Path(args[-1]).write_bytes(b'partial');return child
    monkeypatch.setattr(asyncio,'create_subprocess_exec',spawn)
    with pytest.raises(TimeoutError):
        async with asyncio.timeout(.01):await cache.fragment(media)
    assert child.killed and not media.with_suffix('.fragmented').exists()


def test_static_urls_reuse_cache_and_repopulate_after_eviction_and_restart(client,monkeypatch):
    renders=[];lookups=[]
    async def song(*args):
        lookups.append(args)
        return {**SONG,'attributes':{**SONG['attributes'],'durationInMillis':3500}}
    monkeypatch.setattr('switchmusic.apple.Apple.catalog_song',song)
    raw=box(b'ftyp')+box(b'moov')+box(b'moof')+box(b'mdat',b'audio')
    async def render(song_id,target):renders.append(song_id);target.write_bytes(raw)
    cache=client.app.state.playback;cache.render=render
    url='/v1/playback/123.m3u8'
    first=client.get(url);assert first.status_code==200
    # Expired credentials do not make these public URLs expire.
    assert client.get(url,headers={'Cookie':'switchmusic_session=expired'}).text==first.text
    assert renders==['123'] and len(lookups)==1
    # Recreate the cache object to exercise existing files after a server restart.
    cache=Playback(cache.s,cache.apple);cache.render=render;client.app.state.playback=cache
    assert client.get(url).text==first.text
    assert renders==['123'] and len(lookups)==1
    assert client.get('/v1/stream/123',headers={'Range':'bytes=16-36'}).content==raw[16:37]
    cache.prune(cache.s.max_cache_bytes)
    assert not (cache.s.cache_dir/'123.m4a').exists()
    assert not (cache.s.cache_dir/'123.m3u8').exists()
    assert client.get(url).text==first.text
    assert renders==['123','123'] and len(lookups)==2
    # Direct media requests also prepare missing tracks, without fetching a playlist.
    cache.prune(cache.s.max_cache_bytes)
    response=client.get('/v1/stream/123',headers={'Range':'bytes=16-36'})
    assert response.status_code==206 and response.content==raw[16:37]
    assert renders==['123','123','123'] and cache.active=={}


def test_codec_renditions_keep_separate_cache_and_stream_urls(client,monkeypatch):
    async def song(*args):
        return {**SONG,'attributes':{**SONG['attributes'],'durationInMillis':3500}}
    monkeypatch.setattr('switchmusic.apple.Apple.catalog_song',song)
    renders=[]
    async def render(song_id,target,codec=None):
        renders.append(codec)
        target.write_bytes(box(b'ftyp')+box(b'moov')+box(b'moof')+box(b'mdat',(codec or 'default').encode()))
    cache=client.app.state.playback;cache.render=render
    for codec in (None,'alac','aac','aac-web'):
        query=f'?codec={codec}' if codec else ''
        response=client.get('/v1/playback/123.m3u8'+query)
        assert response.status_code==200,response.text
        assert f'/v1/stream/123{query}\n' in response.text
        assert f'URI="/v1/stream/123{query}"' in response.text
        audio=client.get('/v1/stream/123'+query)
        assert audio.status_code==200
        assert audio.content.endswith((codec or 'default').encode())
        assert client.get('/v1/playback/123.m3u8'+query).text==response.text
    assert renders==[None,'alac','aac','aac-web']
    assert cache.active=={}
    cache.prune(cache.s.max_cache_bytes)
    assert not list(cache.s.cache_dir.glob('*.m4a'))
    assert not list(cache.s.cache_dir.glob('*.m3u8'))

@pytest.mark.parametrize('endpoint',['playback/123.m3u8','stream/123'])
def test_invalid_codec_is_rejected(client,endpoint):
    assert client.get('/v1/'+endpoint+'?codec=atmos').status_code==422
