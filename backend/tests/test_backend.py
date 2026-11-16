import asyncio
import json
import time
from dataclasses import replace
from pathlib import Path

import httpx
import jwt
import pytest
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import serialization
from fastapi import HTTPException
from fastapi.testclient import TestClient

from switchmusic.app import create_app
from switchmusic.settings import Settings
from switchmusic.lyrics import parse_subtitles
from switchmusic.playback import Playback

TOKEN = 'device-' + 'x' * 40
HEADERS = {'Authorization': 'Bearer ' + TOKEN}
SONG = {'id': '123', 'type': 'songs', 'attributes': {'name': 'Song', 'artistName': 'Artist', 'isrc': 'ISRC123'}}

@pytest.fixture
def settings(tmp_path):
    key = ec.generate_private_key(ec.SECP256R1())
    p = tmp_path / 'key.p8'
    p.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    user_token_file = tmp_path / 'music_user_token'
    user_token_file.write_text('USER_SECRET')
    return Settings(TOKEN, 'TEAM', 'KEY', p, user_token_file=user_token_file, wrapper_url='http://wrapper', cache_dir=tmp_path / 'cache')

@pytest.fixture
def client(settings):
    def handler(request):
        if request.url.host == 'wrapper':
            return httpx.Response(200, json={'auth': {'music_user_token': 'USER_SECRET', 'dev_token': 'DO_NOT_USE'}})
        if request.url.host == 'api.music.apple.com':
            assert request.headers['Music-User-Token'] == 'USER_SECRET'
            token = request.headers['Authorization'].removeprefix('Bearer ')
            assert jwt.decode(token, options={'verify_signature': False})['iss'] == 'TEAM'
            if request.url.path.endswith('/catalog') or '/catalog/' in request.url.path:
                return httpx.Response(200, json={'data': [SONG]})
            return httpx.Response(200, json={'data': [dict(SONG, id='i.local', type='library-songs')], 'next': '/v1/me/library/songs?offset=1'})
        raise AssertionError(str(request.url))
    with TestClient(create_app(settings, httpx.MockTransport(handler))) as c:
        yield c

def test_health_no_credentials(client):
    assert client.get('/health').json()['status'] == 'ok'

@pytest.mark.parametrize('path', ['/v1/library/songs', '/v1/resolve/songs/123', '/v1/lyrics/123', '/v1/stream/123'])
def test_protected(client, path):
    assert client.get(path).status_code == 401

def test_official_library_and_pagination(client):
    r = client.get('/v1/library/songs?offset=100', headers=HEADERS)
    assert r.status_code == 200
    assert r.json()['next_offset'] == 101
    assert r.json()['items'][0]['title'] == 'Song'
    assert 'SECRET' not in r.text and 'DO_NOT_USE' not in r.text

@pytest.mark.parametrize('path,code', [('/v1/library/artists',404),('/v1/library/songs?offset=-1',422),('/v1/resolve/albums/123',422),('/v1/stream/not-a-catalog-id',400),('/v1/resolve/songs/bad%3Fid',400)])
def test_validation(client, path, code):
    assert client.get(path, headers=HEADERS).status_code == code

def test_library_catalog_resolution(client):
    r = client.get('/v1/resolve/library-songs/i.local', headers=HEADERS)
    assert r.status_code == 200
    assert r.json()['stream_path'] == '/v1/stream/123'
    assert r.json()['id'] == '123'

def test_lyrics_require_licensed_key(client):
    r = client.get('/v1/lyrics/123', headers=HEADERS)
    assert r.status_code == 503
    assert 'Musixmatch' in r.text

def test_subtitle_timing():
    body=json.dumps([{'time': {'total':2.25}, 'text':'later'}, {'time': {'minutes':0,'seconds':1,'hundredths':5}, 'text':'first'}])
    assert parse_subtitles(body) == [{'time_ms':1050,'text':'first'},{'time_ms':2250,'text':'later'}]

def test_range_requests(client):
    async def render(song_id, target):
        target.write_bytes(b'0123456789')
    client.app.state.playback.render = render
    r = client.get('/v1/stream/123', headers={**HEADERS, 'Range':'bytes=2-5'})
    assert r.status_code == 206
    assert r.content == b'2345'
    assert r.headers['content-range'] == 'bytes 2-5/10'

async def test_cache_coalesces_and_cleans_failed_files(settings):
    cache=Playback(settings,None)
    calls=0
    async def render(song_id,target):
        nonlocal calls
        calls+=1
        await asyncio.sleep(.01)
        target.write_bytes(b'audio')
    cache.render=render
    paths=await asyncio.gather(cache.materialize('123'),cache.materialize('123'))
    assert calls==1 and paths[0]==paths[1]
    async def fail(song_id,target):
        target.write_bytes(b'partial')
        raise RuntimeError('PRIVATE upstream URL')
    cache.render=fail
    with pytest.raises(HTTPException) as e:
        await cache.materialize('456')
    assert 'PRIVATE' not in e.value.detail
    assert not (settings.cache_dir/'456.part').exists()

async def test_cache_ttl_and_budget(settings):
    cache=Playback(replace(settings,max_cache_bytes=10,max_track_bytes=5),None)
    first=settings.cache_dir/'1.m4a';first.write_bytes(b'123456')
    cache.prune(reserve=5)
    assert not first.exists()

async def test_active_cache_file_is_not_evicted(settings):
    cache=Playback(replace(settings,max_cache_bytes=10,max_track_bytes=6),None)
    async def render(song_id,target):
        target.write_bytes(b'123456')
    cache.render=render
    active=await cache.materialize('1',acquire=True)
    with pytest.raises(HTTPException) as exc:
        await cache.materialize('2')
    assert exc.value.status_code==503 and active.exists()
    cache.release(active)
    await cache.materialize('2')
    assert not active.exists()

def test_range_response_releases_cache_lease(client):
    async def render(song_id,target):
        target.write_bytes(b'0123456789')
    client.app.state.playback.render=render
    assert client.get('/v1/stream/123', headers={**HEADERS,'Range':'bytes=999-1000'}).status_code==416
    assert client.app.state.playback.active=={}

def test_missing_musickit_authorization(settings):
    settings.user_token_file.unlink()
    with TestClient(create_app(settings)) as c:
        r=c.get('/v1/library/songs',headers=HEADERS)
        assert r.status_code==503
        assert 'authorization helper' in r.text

def test_local_musickit_helper_saves_private_token(settings, tmp_path):
    import re
    import socket
    import stat
    import subprocess
    import sys
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0))
        port=sock.getsockname()[1]
    output=tmp_path/'authorized-token'
    helper=Path(__file__).resolve().parents[2]/'scripts/authorize-musickit.py'
    process=subprocess.Popen([sys.executable,str(helper),'--key',str(settings.private_key),
        '--team-id','TEAM','--key-id','KEY','--port',str(port),'--output',str(output)],
        stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    url=f'http://127.0.0.1:{port}'
    try:
        for attempt in range(100):
            try:
                page=httpx.get(url+'/',timeout=1)
                if page.status_code==200:
                    break
            except httpx.TransportError:
                pass
            time.sleep(.02)
        else:
            raise AssertionError('MusicKit helper did not start')
        csrf=json.loads(re.search(r"'X-SwitchMusic-CSRF':(\"[^\"]+\")",page.text)[1])
        developer=json.loads(re.search(r'developerToken:(\"[^\"]+\")',page.text)[1])
        claims=jwt.decode(developer,options={'verify_signature':False})
        assert claims['exp']-claims['iat']==900
        assert 'PRIVATE KEY' not in page.text
        r=httpx.post(url+'/token',headers={'Origin':'https://wrong.example','X-SwitchMusic-CSRF':csrf},json={'token':'test-user-token'})
        assert r.status_code==403 and not output.exists()
        r=httpx.post(url+'/token',headers={'Origin':url,'X-SwitchMusic-CSRF':csrf},json={'token':'test-user-token'})
        assert r.status_code==200
        assert output.read_text()=='test-user-token'
        assert stat.S_IMODE(output.stat().st_mode)==0o600
        assert process.wait(timeout=5)==0
    finally:
        if process.poll() is None:
            process.terminate();process.wait(timeout=5)
