import asyncio
import base64
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
from switchmusic.lyrics import parse_lrc
from switchmusic.playback import Playback

PASSWORD = 'Admin-password-12345'
HEADERS = {'Authorization': 'Basic '+base64.b64encode(('admin:'+PASSWORD).encode()).decode()}
SONG = {'id': '123', 'type': 'songs', 'attributes': {'name': 'Song', 'artistName': 'Artist', 'isrc': 'ISRC123'}}

@pytest.fixture
def settings(tmp_path):
    key = ec.generate_private_key(ec.SECP256R1())
    p = tmp_path / 'key.p8'
    p.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    return Settings(PASSWORD, 'TEAM123456', 'KEY1234567', p, data_dir=tmp_path/'data', public_url='http://testserver', secure_cookies=False, wrapper_url='http://wrapper', cache_dir=tmp_path/'cache')

@pytest.fixture
def client(settings):
    def handler(request):
        if request.url.host == 'wrapper':
            return httpx.Response(200, json={'auth': {'music_user_token': 'USER_SECRET', 'dev_token': 'DO_NOT_USE'}})
        if request.url.host == 'api.music.apple.com':
            if '/me/' in request.url.path:
                assert request.headers['Music-User-Token'] == 'USER_SECRET'
            else:
                assert 'Music-User-Token' not in request.headers
            token = request.headers['Authorization'].removeprefix('Bearer ')
            assert jwt.decode(token, options={'verify_signature': False})['iss'] == 'TEAM123456'
            if request.url.path.endswith('/catalog') or '/catalog/' in request.url.path:
                return httpx.Response(200, json={'data': [SONG]})
            return httpx.Response(200, json={'data': [dict(SONG, id='i.local', type='library-songs')], 'next': '/v1/me/library/songs?offset=1'})
        raise AssertionError(str(request.url))
    with TestClient(create_app(settings, httpx.MockTransport(handler))) as c:
        c.app.state.accounts.link(1,'USER_SECRET','us')
        yield c

def test_health_no_credentials(client):
    assert client.get('/health').json()['status'] == 'ok'

@pytest.mark.parametrize('path', ['/v1/library/songs', '/v1/resolve/songs/123', '/v1/lyrics/123'])
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

def test_lyrics_without_api_key(client, monkeypatch):
    async def no_apple(*args):return None
    monkeypatch.setattr('switchmusic.lyrics.apple_lrc',no_apple)
    calls=[]
    def search(query, **kwargs):
        calls.append((query,kwargs))
        return '[00:01.05]first\n[00:02.250]later'
    monkeypatch.setattr('switchmusic.lyrics.syncedlyrics.search',search)
    r=client.get('/v1/lyrics/123',headers=HEADERS)
    assert r.status_code==200
    assert r.json()['provider']=='syncedlyrics'
    assert r.json()['lines']==[{'time_ms':1050,'text':'first'},{'time_ms':2250,'text':'later'}]
    assert calls==[('Song Artist',{'synced_only':True})]

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

def test_missing_musickit_authorization(client):
    client.app.state.accounts.unlink(1)
    r=client.get('/v1/library/songs',headers=HEADERS)
    assert r.status_code==409
    assert 'website' in r.text


def test_anonymous_catalog_browsing_never_uses_personal_token(settings):
    calls=[]
    def upstream(request):
        calls.append(request)
        assert 'Music-User-Token' not in request.headers
        assert request.headers['Authorization'].startswith('Bearer ')
        assert request.url.path.startswith('/v1/catalog/us/')
        if request.url.path.endswith('/search'):
            return httpx.Response(200,json={'results':{'songs':{'data':[SONG]}}})
        return httpx.Response(200,json={'data':[SONG],'next':'next'})
    with TestClient(create_app(settings,httpx.MockTransport(upstream))) as client:
        assert client.get('/v1/catalog/search?term=test').json()['items'][0]['id']=='123'
        for kind in ('albums','playlists'):
            response=client.get(f'/v1/catalog/{kind}/123/tracks?offset=100')
            assert response.status_code==200
            assert response.json()['next_offset']==101
        for path in ('/v1/library/songs','/v1/library/albums/123/tracks','/auth/me','/v1/resolve/songs/123'):
            assert client.get(path).status_code==401
        assert len(calls)==3

def test_route_prefers_apple_lyrics(client, monkeypatch):
    async def primary(song,settings):
        assert song['id']=='123' and settings.wrapper_url=='http://wrapper'
        return '[00:01.250]Apple line'
    def forbidden(*args,**kwargs):raise AssertionError('Fallback must not be called')
    monkeypatch.setattr('switchmusic.lyrics.apple_lrc',primary)
    monkeypatch.setattr('switchmusic.lyrics.syncedlyrics.search',forbidden)
    response=client.get('/v1/lyrics/123',headers=HEADERS)
    assert response.status_code==200
    assert response.json()=={'provider':'apple-music','copyright':'','tracking_url':'','lines':[{'time_ms':1250,'text':'Apple line'}]}


def test_library_resolution_fetches_individual_public_track(settings):
    calls=[]
    def upstream(request):
        calls.append(request.url.path)
        if request.url.path=='/v1/me/library/songs/i.local/catalog':
            assert request.headers['Music-User-Token']=='LINKED_ACCOUNT'
            # Mapping data must not be reused as the playback metadata.
            return httpx.Response(200,json={'data':[{**SONG,'attributes':{'name':'Library mapping'}}]})
        assert request.url.path=='/v1/catalog/us/songs/123'
        assert 'Music-User-Token' not in request.headers
        return httpx.Response(200,json={'data':[SONG]})
    with TestClient(create_app(settings,httpx.MockTransport(upstream))) as client:
        client.app.state.accounts.link(1,'LINKED_ACCOUNT','us')
        response=client.get('/v1/resolve/library-songs/i.local',headers=HEADERS)
        assert response.status_code==200,response.text
        assert response.json()['title']=='Song'
        assert response.json()['type']=='songs'
        assert response.json()['playback_path']=='/v1/playback/123.m3u8'
        assert response.json()['stream_path']=='/v1/stream/123'
    assert calls==['/v1/me/library/songs/i.local/catalog','/v1/catalog/us/songs/123']

@pytest.mark.parametrize('matches',[[],[{'id':'i.local','type':'library-songs'}]])
def test_unmapped_library_song_never_reaches_playback(settings,matches):
    def upstream(request):
        assert request.url.path=='/v1/me/library/songs/i.local/catalog'
        return httpx.Response(200,json={'data':matches})
    with TestClient(create_app(settings,httpx.MockTransport(upstream))) as client:
        client.app.state.accounts.link(1,'LINKED_ACCOUNT','us')
        assert client.get('/v1/resolve/library-songs/i.local',headers=HEADERS).status_code==422
        assert not list(settings.cache_dir.iterdir())
