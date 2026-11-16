from io import BytesIO
import httpx
import pytest
from PIL import Image
from fastapi.testclient import TestClient
from switchmusic.app import create_app
from switchmusic.apple import normalize
from test_backend import settings,HEADERS

def png(color=(255,0,0)):
    out=BytesIO();Image.new('RGB',(32,32),color).save(out,format='PNG');return out.getvalue()

def test_artwork_variants_cache_and_no_credentials_to_cdn(settings):
    calls=[]
    def upstream(request):
        calls.append(request)
        if request.url.host=='api.music.apple.com':
            assert 'Authorization' in request.headers
            return httpx.Response(200,json={'data':[{'id':'123','type':'songs','attributes':{'artwork':{'url':'https://is1-ssl.mzstatic.com/image/thumb/fixture/{w}x{h}bb.jpg'}}}]})
        assert request.url.host=='is1-ssl.mzstatic.com'
        assert 'Authorization' not in request.headers and 'Music-User-Token' not in request.headers and 'Cookie' not in request.headers
        return httpx.Response(200,content=png())
    with TestClient(create_app(settings,httpx.MockTransport(upstream))) as client:
        image=client.get('/v1/artwork/songs/123?size=64');assert image.status_code==200
        assert Image.open(BytesIO(image.content)).size==(64,64)
        assert client.get('/v1/artwork/songs/123?size=64').content==image.content
        raw=client.get('/v1/artwork/songs/123?size=256&format=rgba4444')
        assert raw.status_code==200 and len(raw.content)==256*256*2
        assert raw.content[:2]==b'\xf0\x0f'  # libultrahand RG, BA nibble order
        assert len(calls)==3  # metadata once, two CDN sizes
        assert not client.app.state.artwork.bytes>8*1024*1024
        assert client.get('/v1/artwork/songs/123?size=1024').status_code==400
        assert client.get('/v1/artwork/songs/123?size=64&format=rgba4444').status_code==400

def test_normalized_artwork_is_backend_relative():
    result=normalize({'id':'123','type':'songs','attributes':{'name':'Title','artwork':{'url':'https://private.example/template'}}})
    assert result['artwork_path']=='/v1/artwork/songs/123'
    assert 'private.example' not in str(result)

@pytest.mark.parametrize('url',['http://is1-ssl.mzstatic.com/a','https://example.com/a','https://is1-ssl.mzstatic.com.evil/a','https://user:secret@is1-ssl.mzstatic.com/a',''])
def test_artwork_rejects_non_apple_sources(settings,url):
    def upstream(request):
        assert request.url.host=='api.music.apple.com'
        return httpx.Response(200,json={'data':[{'attributes':{'artwork':{'url':url}}}]})
    with TestClient(create_app(settings,httpx.MockTransport(upstream))) as client:
        assert client.get('/v1/artwork/songs/123').status_code==404

def test_personal_artwork_requires_linked_account_even_when_cached(settings):
    def upstream(request):
        if request.url.host=='api.music.apple.com':
            assert request.headers['Music-User-Token']=='PRIVATE_TOKEN'
            return httpx.Response(200,json={'data':[{'attributes':{'artwork':{'url':'https://is1-ssl.mzstatic.com/library/{w}x{h}.jpg'}}}]})
        return httpx.Response(200,content=png())
    with TestClient(create_app(settings,httpx.MockTransport(upstream))) as client:
        path='/v1/artwork/library-songs/i.123'
        assert client.get(path).status_code==401
        assert client.get(path,headers=HEADERS).status_code==409
        client.app.state.accounts.link(1,'PRIVATE_TOKEN','us')
        assert client.get(path,headers=HEADERS).status_code==200
        client.app.state.accounts.unlink(1)
        assert client.get(path,headers=HEADERS).status_code==409
        assert client.get(path).status_code==401
